/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/



#include "co_routine.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#ifdef __FreeBSD__
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#endif

using namespace std;

//一个协程对应一个fd，一个线程最多支持128协程
struct task_t
{
	stCoRoutine_t *co;
	int fd;
};

static stack<task_t*> g_readwrite;
static int g_listen_fd = -1;
static int SetNonBlock(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

//执行这个函数时控制权已经交给了新创建的协程了，用户可以自定义此函数的功能
static void *readwrite_routine( void *arg )
{
	//printf("readwrite_route\n");
	//fflush(stdout);
	co_enable_hook_sys();

	task_t *co = (task_t*)arg;
	char buf[ 1024 * 16 ];
	
	for(;;)
	{
		//在外层循环中将新创建的读写协程co加入到 g_readwrite 中，此时这些读写协程都没有具体与某个 socket 连接对应，可以将g_readwrite 看成一个 coroutine pool。
		//当加入到栈中之后，调用函数 co_yield_ct 函数让出 CPU，此时控制权回到主协程。
		if( -1 == co->fd )
		{
			g_readwrite.push( co );
			co_yield_ct();
			continue;
		}
		//co->fd不为-1：这个fd就是新加入的连接。
		int fd = co->fd;
		co->fd = -1;

		for(;;)
		{
			//调用 co_poll 将新建立的连接的 fd 加入到 Epoll 监听中
			struct pollfd pf = { 0 };
			pf.fd = fd;
			pf.events = (POLLIN|POLLERR|POLLHUP);
			co_poll( co_get_epoll_ct(),&pf,1,1000);

			int ret = read( fd,buf,sizeof(buf) );
			if( ret > 0 )
			{
				ret = write( fd,buf,ret );
			}
			if( ret > 0 || ( -1 == ret && EAGAIN == errno ) )
			{
				continue;
			}
			close( fd );
			break;
		}

	}
	return 0;
}
int co_accept(int fd, struct sockaddr *addr, socklen_t *len );

//由协程accept_co 执行的函数，应该无需改动
static void *accept_routine( void * )
{
	co_enable_hook_sys();
	printf("accept_routine\n");
	fflush(stdout);
	for(;;)
	{
		//printf("pid %ld g_readwrite.size %ld\n",getpid(),g_readwrite.size());
		//此if相当于sleep(1000) 1s
		if( g_readwrite.empty() )
		{
			printf("empty\n"); //sleep 
			struct pollfd pf = { 0 };
			pf.fd = -1;
			poll( &pf,1,1000);

			continue;

		}
		struct sockaddr_in addr; //maybe sockaddr_un;
		memset( &addr,0,sizeof(addr) );
		socklen_t len = sizeof(addr);

		//接受这个连接，返回一个已连接套接字fd
		int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
		//如果接收连接失败，那么调用 co_poll 将服务端的 listen_fd 再次加入到 Epoll 中来触发下一次连接事件；
		if( fd < 0 )
		{
			struct pollfd pf = { 0 };
			pf.fd = g_listen_fd;
			pf.events = (POLLIN|POLLERR|POLLHUP);
			co_poll( co_get_epoll_ct(),&pf,1,1000 );
			continue;
		}
		//为什么又判断一次呢
		if( g_readwrite.empty() )
		{
			close( fd );
			continue;
		}
		//对于成功的连接，从 g_readwrite 中取出一个读写协程来负责处理读写
		SetNonBlock( fd );
		task_t *co = g_readwrite.top();
		co->fd = fd;
		g_readwrite.pop();
		co_resume( co->co );//该协程运行直至挂起
	}
	return 0;
}

static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP   
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
		|| 0 == strcmp(pszIP,"*") 
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;

}

static int CreateTcpSocket(const unsigned short shPort /* = 0 */,const char *pszIP /* = "*" */,bool bReuse /* = false */)
{
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			SetAddr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}


int main(int argc,char *argv[])
{
	if(argc<5){
		printf("Usage:\n"
               "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT]\n"
               "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT] -d   # daemonize mode\n");
		return -1;
	}
	//表明有proccnt个子进程，proccnt个accept协程，proccnt*cnt 个 readwrite协程。
	const char *ip = argv[1];
	int port = atoi( argv[2] );
	int cnt = atoi( argv[3] );
	int proccnt = atoi( argv[4] );//启用的进程数！
	bool deamonize = argc >= 6 && strcmp(argv[5], "-d") == 0;

	g_listen_fd = CreateTcpSocket( port,ip,true );//设置为reuseaddr
	listen( g_listen_fd,1024 );
	if(g_listen_fd==-1){
		printf("Port %d is in use\n", port);
		return -1;
	}
	printf("listen %d %s:%d\n",g_listen_fd,ip,port);

	SetNonBlock( g_listen_fd );

	for(int k=0;k<proccnt;k++)
	{

		pid_t pid = fork();
		if( pid > 0 )
		{
			continue;
		}
		else if( pid < 0 )
		{
			break;
		}
		//接下来是每个子进程做的事情，每个子进程创建了cnt个读写协程
		for(int i=0;i<cnt;i++)
		{
			task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
			task->fd = -1;

			//将自定义的读写函数readwrite_routine交由协程执行，如同线程pthead_create里传入的将要运行的函数一样！
			//task就是readwrite_routine函数的参数，co_create的返回值由task->co接管
			co_create( &(task->co),NULL,readwrite_routine,task );//创建读写协程
			co_resume( task->co );

		}
		//每个子进程的主线程继续创建 接受协程accpet_co，运行接受函数accept_routine，同样调用 co_resume 启动协程直到其挂起；
		stCoRoutine_t *accept_co = NULL;
		co_create( &accept_co,NULL,accept_routine,0 );
		co_resume( accept_co );

		//主线程继续调用函数 co_eventloop 实现事件的监听和协程的循环切换，如监听到新连接则交由accept_co协程处理
		co_eventloop( co_get_epoll_ct(),0,0 );

		exit(0);
	}
	//父进程只是等待所有子进程的终止而已
	if(!deamonize) wait(NULL);
	return 0;
}

