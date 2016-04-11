#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "hiredis.h"
#include "ae.h"
#include "adlist.h"
#include "sds.h"
#include <unistd.h>  
#include <pthread.h>
#include <curl/curl.h>
#include <curl/multi.h>
#include "cJSON.h"  
int cocurrent=10;
char * hostip="127.0.0.1";
char * queen_key="queen";
int hostport = 6379;
redisContext *c;
list * queen_list;
pthread_t *queen_thread;
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition=PTHREAD_MUTEX_INITIALIZER;

void parseOptions(int argc, const char **argv) {
    int i;
    int lastarg;
    int exit_status = 1;

    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc-1));

        if (!strcmp(argv[i],"-c")) {
            if (lastarg) goto invalid;
            cocurrent = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-h")) {
            if (lastarg) goto invalid;
            hostip = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-p")) {
            if (lastarg) goto invalid;
            hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-k")){
	    if (lastarg) goto invalid;
	    queen_key=argv[++i];
	} else if (!strcmp(argv[i],"--help")) {
            exit_status = 0;
            goto usage;
        } else {
            /* Assume the user meant to provide an option when the arg starts
             * with a dash. We're done otherwise and should use the remainder
             * as the command and arguments for running the benchmark. */
            if (argv[i][0] == '-') goto invalid;
            return ;
        }
    }
    return;
invalid:
    printf("Invalid option \"%s\" or option argument missing\n\n",argv[i]);

usage:
    printf(
"Usage: queen-daemon [-h <host>] [-p <port>] [-c <cocurrent>] [-k <queen_key>]\n\n"
" -h <hostname>      Server hostname (default 127.0.0.1)\n"
" -p <port>          Server port (default 6379)\n"
" -c <cocurrent>     Number of cocurrent (default 10)\n"
" -k <queen_key>    The queen key name\n"

"Examples:\n\n"
" Run the benchmark with the default configuration against 127.0.0.1:6379:\n"
"   $ queen-deamon\n\n"
" Use 20 cocurrent, queen key is mylis ,against 192.168.1.1:\n"
"   $ redis-benchmark -h 192.168.1.1 -p 6379 -c 20 -k mylist\n\n"
    );
    exit(exit_status);
}
int eventLoop()
{
    //如果队列数据过多，推出
    pthread_mutex_lock(&mutex);
    if(queen_list->len>5000){
    	pthread_mutex_unlock(&mutex);
    	return 1000;
   	}	
    unsigned int j;
    redisReply *reply;
	int max=(int)time(NULL);
    reply = (redisReply*) redisCommand(c,"zrange mylist 0 %d",max);
    if(reply){
    	if (reply->type == REDIS_REPLY_ARRAY) {
    		pthread_mutex_lock(&mutex); 
        	for (j = 0; j < reply->elements; j++) {
            	//printf("%u) %s\n", j, reply->element[j]->str);
            	//插入队列
            	listAddNodeTail(queen_list, sdsnew(reply->element[j]->str));
        	}
        	pthread_mutex_unlock(&mutex);
        	pthread_cond_signal(&condition);
        	freeReplyObject(reply);
        	//删除
        	reply = (redisReply*) redisCommand(c,"ZREMRANGEBYSCORE mylist 0 %d",max);
        	if(reply){
        		if(reply == (void*)REDIS_REPLY_ERROR) {
              		fprintf(stderr,"Unexpected error reply, exiting...\n");
            	}
            	freeReplyObject(reply);
            }else{
            	printf("%s\n","delete error");
            }
        }
    }else{
    	printf("%s\n","list empty");
    }
    return 1000;

}
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata){
    (void)ptr;  /* unused */
    (void)userdata; /* unused */
    return (size_t)(size * nmemb);
}
void *queen_maintenance_thread(void *arg){
        curl_global_init(CURL_GLOBAL_ALL);
        CURLcode res;
        CURL * curl_handle = curl_easy_init();
        while(1){
            char fields[500]="";
            pthread_mutex_lock(&mutex); 
            while(queen_list->len==0)
            {
                pthread_cond_wait(&condition, &mutex);
            }
            listNode *node=queen_list->head;
            sds content=sdsnew(node->value);
            listDelNode(queen_list,node);
            pthread_mutex_unlock(&mutex);
            printf("%s\n",content);
            
            //curl excuation
            cJSON *json;
            json=cJSON_Parse(content);
            if (!json) {
                printf("Error before: [%s]\n",cJSON_GetErrorPtr());
                continue;
            }
            char url[200]="";
            sprintf(url,cJSON_GetObjectItem(json,"url")->valuestring);
            if(!strlen(url)){
                printf("Error before: [%s]\n",cJSON_GetErrorPtr());
                continue;
            }
            // cJSON_Delete(json);
            // sdsfree(content);

            // continue;
            
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
            char * method="get";
            if(cJSON_GetObjectItem(json,"method")){
                method=cJSON_GetObjectItem(json,"method")->valuestring;
            }
            
            cJSON * params=cJSON_GetObjectItem(json,"params");
            if(params){
               // printf("%s\n",cJSON_Print(params));
                cJSON * child=params->child;
                while(child){
                    sprintf(fields+strlen(fields),"%s=%s&",child->string,child->valuestring);
                    child=child->next;
                }
                fields[strlen(fields)-1]=0;
                printf("%s\n",fields);
            }
            
            if(method&&!strcasecmp(method,"post")){
                curl_easy_setopt(curl_handle, CURLOPT_POST, 1);
                curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, fields); 
            }else{
                sprintf(url+strlen(url),"?");
                sprintf(url+strlen(url),fields);
            }
            curl_easy_setopt(curl_handle, CURLOPT_URL, url);
            res = curl_easy_perform(curl_handle);
            if(CURLE_OK == res) {
                char *ct;
                res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
                if((CURLE_OK == res) && ct)
                printf("We received Content-Type: %s\n", ct);
            }else{
                printf("%s\n","error");
            }
            cJSON_Delete(json);
            sdsfree(content);
        }
        curl_easy_cleanup(curl_handle);
        return NULL;
} 
int main(int argc,char ** argv) {
    parseOptions(argc,argv);
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    c = redisConnectWithTimeout(hostip, hostport, timeout);
    if (c->err) {
        printf("Connection error: %s\n", c->errstr);
        exit(1);
    }
    aeEventLoop *ae=aeCreateEventLoop(100);
    aeCreateTimeEvent(ae, 1, eventLoop, NULL, NULL);
    queen_list=listCreate();
    queen_thread=(pthread_t *) malloc(sizeof(pthread_t)*cocurrent);
    for(int i=0;i<cocurrent;i++){
        pthread_create(queen_thread+i, NULL,queen_maintenance_thread, NULL);
    }
    aeMain(ae);
    aeDeleteEventLoop(ae);
}
