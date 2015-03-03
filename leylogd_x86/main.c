//============================================================================
// Name       	: main.c
// Author      	: Christopher Ley
// Version     	: 1.1.1
// Project	   	: leylogd_x86 [beta (untested) 02/03/2015]
// Created     	: 24/02/15
// Modified    	: 02/03/15
// Copyright   	: Do not modify without express permission from the author
// Description 	: main file for [leylogd_x86] daemon process x86 variant
// Notes	   	: Version 1.0 first stable;
//				-Implemented all init.d handlers and interrupts
//				-Implemented timer handlers as skeleton for data sampling
//			   	: Version 1.1.x Current development
//============================================================================
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "become_daemon.h"
// For sockets
#include <sys/un.h>
#include <sys/socket.h>
#include <ctype.h>
//for poll - requires <time.h> as well
#include <poll.h>

#define BUF_SIZE 64 	/* Maximum size of messages exchanged between client
							and server */
#define SV_SOCK_PATH "/tmp/soc_leylogd"
/**************************** LOGGING FUNCTIONS  **************/
/****** Static File Pointers ******/
static FILE *logfp;		/* Log file stream */
static const char *LOG_FILE = "/var/log/leyld.log";
static const char *CONFIG_FILE = "/etc/leylogd/leyld.conf";

/****** Message Loggers ******/
/* Log Message */
static void logMessage(const char *format,...)
{
	va_list argList;
	const char *TIMESTAMP_FMT = "%F %X";	/* = YYYY-MM-DD HH:MM:SS */
#define TS_BUF_SIZE sizeof("YYYY-MM-DD HH:MM:SS")	/* Includes '\0' */
	char timestamp[TS_BUF_SIZE];
	time_t t;
	struct tm *loc;

	t = time(NULL);
	loc = localtime(&t);
	if (loc == NULL || strftime(timestamp, TS_BUF_SIZE, TIMESTAMP_FMT, loc) == 0)
		fprintf(logfp, "??Unknown time??: ");
	else
		fprintf(logfp, "%s: ", timestamp);

	va_start(argList, format); /* stdarg.h macro */
	vfprintf(logfp, format, argList);
	fprintf(logfp, "\n");
	va_end(argList);
}
/* Open Log file */
static void logOpen(const char *logFilename)
{
	mode_t m; /*mode of file*/

	m = umask(077); /* File mode creation mask */
	logfp = fopen(logFilename, "a");
	umask(m);

	if(logfp == NULL)
		exit(EXIT_FAILURE);
	setbuf(logfp, NULL); /* Disable stdio buffering */

//	logMessage("Opened log file");
}
/* Close Log file */
static void logClose(void)
{
	logMessage("Closing log file");
	fclose(logfp);
}
static void readConfigFile(const char *configFilename)
{
	FILE *configfp;
#define SBUF_SIZE 100
	char str[SBUF_SIZE];

	configfp = fopen(configFilename, "r");
	if(configfp != NULL) {	/* Ignore nonexistent file */
		if (fgets(str, SBUF_SIZE, configfp) == NULL)
			str[0] = '\0';
		else
			str[strlen(str) - 1] = '\0';	/* Strip tailing */
		logMessage("Read config file: %s", str);
		fclose(configfp);
	}
}
/**************************************************************/

/**************************** INTERRUPT HANDLERS **************/
/****** Atomic interrupt flags ******/
/* Set nonzero on receipt of interrupt,set as volatile so that the compiler
 * dosen't store as a register value and helps with re-entrancy issues, the
 * atomic identifier ensures the global flag is atomic,i.e. can be changed
 * in one clock cycle! */
static volatile sig_atomic_t termReceived = 0;
static volatile sig_atomic_t alrmReceived = 0;
static volatile sig_atomic_t hupReceived = 0;

/****** Interrupt Handler Function ******/
static void interruptHandler(int sig)
{
    /* A Quick interrupt handler to try to avoid re-entrancy */
    switch(sig)
    {
        case SIGHUP:
            hupReceived = 1;
            break;
        case SIGINT:
        case SIGTERM:
            termReceived = 1;
            break;
        case SIGALRM:
            alrmReceived = 1;
            break;
    }
}
/**************************************************************/

/**************************** MAIN ****************************/
int main(int argc, char *argv[])
{
/* Set up interrupt handler */
	struct sigaction act; // defined by signal.h
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = interruptHandler;
	/* signals to handle */
	sigaction(SIGHUP, &act, NULL); // catch hangup signal
	sigaction(SIGTERM, &act, NULL); // catch terminate (--stop|stop) signal
	sigaction(SIGINT, &act, NULL); // catch Ctrl=C signal
	sigaction(SIGALRM, &act, NULL);// catch timer alarm

/* Set up Daemon Process */
	if(becomeDaemon(0) == -1){
		logMessage("Daemonise Failure");
		exit(EXIT_FAILURE);
	}

/* Open Log file */
	logOpen(LOG_FILE);
	readConfigFile(CONFIG_FILE);
	int count;
	if (argc > 1){
		for(count = 1; count < argc; count++){
			 logMessage(argv[count]);
		}
	}

/* Set up Timer */
	struct itimerval itv;
	/* Set timer values*/
	int sampling_period_s = 30;
	int sampling_period_us = 0;
	 itv.it_value.tv_sec = sampling_period_s;
	 itv.it_value.tv_usec = sampling_period_us;
	 itv.it_interval.tv_sec = sampling_period_s;
	 itv.it_interval.tv_usec = sampling_period_us;
	if (setitimer(ITIMER_REAL, &itv, 0) == -1){
			logMessage("Timer Failure");
			exit(EXIT_FAILURE);
		 }

/* Socket server setup */
	struct sockaddr_un svaddr, claddr;
	int socketfd; // socket file discription
	ssize_t numBytes;
	socklen_t len;
	char buf[BUF_SIZE];
	socketfd = socket(AF_UNIX, SOCK_DGRAM, 0);       /* Create UNIX server socket */

	if (socketfd == -1){
		logMessage("Socket creation failure");
		exit(EXIT_FAILURE);
	}

	if (remove(SV_SOCK_PATH) == -1){
		/* Construct well-known address and bind server socket to it */
		logMessage("Error: remove-%s", SV_SOCK_PATH);
	}
	memset(&svaddr, 0, sizeof(struct sockaddr_un));
	svaddr.sun_family = AF_UNIX;
	strncpy(svaddr.sun_path, SV_SOCK_PATH, sizeof(svaddr.sun_path) - 1);

	if (bind(socketfd, (struct sockaddr *) &svaddr, sizeof(struct sockaddr_un)) == -1){
		logMessage("Error: Binding");
		exit(EXIT_FAILURE);
	}
/* Poll Setup */
	struct pollfd poller_fd;
	int data_available = 0;
	poller_fd.fd = socketfd;
	poller_fd.events = POLLIN;

	/* Final Message b4 loop*/
	logMessage("Initialised");

	for(;;){ /*ever*/
		if(termReceived != 0){
			//TODO close file pointer for logging
			termReceived = 0;
			logClose();
			exit(EXIT_SUCCESS);
		}else if(alrmReceived != 0){
			logMessage("Logging Data...");
			alrmReceived = 0;
			//TODO Data logging
		}else if(hupReceived != 0){
			logMessage("Hang-up Received");
			readConfigFile(CONFIG_FILE);
			hupReceived = 0;
			//TODO Re-initialise parameters
		}else{
			int timeout = (int) (sampling_period_s*1000/2); // timeout for half the sampling period
			logMessage("Value of timeout %6.2f",timeout);
			data_available = poll(&poller_fd, 1, timeout);
			if (data_available != 0){
				if(data_available == -1){
					logMessage("Poll Error");
				} else {
					len = sizeof(struct sockaddr_un);
					numBytes = recvfrom(socketfd, buf, BUF_SIZE, 0,
						        		(struct sockaddr *) &claddr, &len);
					if(numBytes == -1){
						logMessage("Read Error");
					} else {
						logMessage("Server received: %s",numBytes);
					}
				}
			}
			pause(); /* suspend until a signal is received.*/
		}
	}
	exit(EXIT_SUCCESS);
}
