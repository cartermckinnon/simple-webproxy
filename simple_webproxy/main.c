/*
 |  A simple multi-threaded HTTP server.
 |  Created by Carter McKinnon on 10/25/16.
 |
 | THANKS:
 | - socket library overview-> http://www.linuxhowtos.org/C_C++/socket.htm
 | - strnstr() implementation-> http://stackoverflow.com/questions/23999797/implementing-strnstr
 |    ^ Creative Commons license.
 | - mobile User-Agents-> http://www.zytrax.com/tech/web/mobile_ids.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <limits.h>
#include<arpa/inet.h>

//#include "strnstr.c"      // necessary for many Linux systems.
// (BSD varients often implement strnstr() in the standard C lib.)

/* _____ OPTIONS _____ */
#define NUM_THREADS 4       // number of threads, number of simultaneous requests handled.
/* (Because no connection is persistant, a higher number will result
 in increased performance for a single client and for multiple clients,
 up to a point. Optimal number would be the number of hardware threads. ) */

#define BUFFER_SIZE 1024     // size of client buffer in bytes, minimum 1024 to capture long headers
#define TIMEOUT_SEC 1
#define TIMEOUT_MICROSEC 0


/* _____ GLOBALS _____ */
static int port;                                               // port number
static int sock, cli_addr_len;                 // socket handle;
static int connections[NUM_THREADS*2];                         // connection handles (1 for cli, 1 for serv)
static char* buffers[NUM_THREADS*2];                             // connection buffers
static struct sockaddr_in serv_addr,
cli_addr[NUM_THREADS];
                          //host_addr[NUM_THREADS];               // server, client, & remote host addresses
//static struct hostent remote_host[NUM_THREADS];
static int online;                                             // status of server
static int mobile;
static int redirect;
static struct timeval timeout = {TIMEOUT_SEC, TIMEOUT_MICROSEC};

static char* redirect_host;
static pthread_t threads[NUM_THREADS];                         // thread handles
static int session[NUM_THREADS];                                 // client session id's
static pthread_mutex_t id_mutex,
                       online_mutex,
                       mobile_mutex,
                       redirect_mutex;                                  // thread syncronization
char* mobile_ids;                                              // list of mobile 'User-Agent' id's


/* _____ FUNCTIONS _____ */
void error(char *msg);                          // error handler
int add_client();                               // allocate client id
void sub_client();                              // deallocate client id
void parse_args(int argc, const char* argv[] ); // parse command line arguments
void prepare_socket();                          // create & bind socket
void accept_connections();                      // listen for & deploy connections to threads
void* serve(void* client);                      // serve client (thread workload)
void connect_host(int id);
void grab_host(int id);
int grab_port(int id);
int contains_close(int id);
char* get_header(int id, char* packet);             // generate header for response
char* header_helper(int id,
                    int status_code,
                    char* status_msg,
                    char* content_type,
                    int packet_length,
                    int filename_length);       // prints header to thread's buffer
char* redirect_helper(int id, char* url);       // prints status 301 header to thread's buffer
void relay_request(int client); // send header to client
int relay_response(int client);             // send data from (templated) file to client
int file_open(char* file);                      // open file for reading
int file_size(char* file);                      // get file size in bytes
int file_exists(char* file);                    // check if file exists
void file_build_path(int id, char* packet);     // combine root directory w/ requested file's path in thread's buffer
void file_build_mobile_path(int id, char* packet); // combine root directory + "/mobile" + file name in thread's buffer
char* url_build(int id, char* packet);           // build url in thread's buffer
void go_offline();                              // first step in exit--break all loops
int is_online();                                // check online status
void load_mobile_ids(char* filename);           // load list of mobile User-Agents into mobile_ids
int mobile_user(char* packet);                  // determine whether a packet uses a supported mobile User-Agent
void cleanup_and_exit();                        // go_offline, cleanup memory, join threads, and exit (called w/ CTRL+C)

int main(int argc, const char * argv[])
{
    /* parse args */
    parse_args(argc, argv);
    
    /* register exit signal (CTRL+C) */
    signal(SIGINT, cleanup_and_exit);
    
    /* initialize mutex */
    pthread_mutex_init(&id_mutex, NULL);
    pthread_mutex_init(&online_mutex, NULL);
    pthread_mutex_init(&mobile_mutex, NULL);
    pthread_mutex_init(&redirect_mutex, NULL);

    
    /* initialize client ID's */
    for( int i = 0; i < NUM_THREADS; i++){
        session[i] = NUM_THREADS;
    }
    
    /* prepare socket */
    prepare_socket();
    
    /* accept connections (loop) */
    accept_connections();
}

/* parse CLI arguments */
void parse_args(int argc, const char* argv[] )
{
    // check number of args
    if( argc != 2 ) error("usage: ./proxy <port #>\n");
    
    // check if port in range
    port = atoi(argv[1]);
    if( port <= 0 ) error("port number must be greater than zero.\n");
}

/* create & bind socket */
void prepare_socket()
{
    /* create socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if( sock < 0 ) error("Couldn't create socket");
    
    /* prepare tools */
    bzero((char *) &serv_addr, sizeof(serv_addr));  // initialize to zero
    serv_addr.sin_family = AF_INET;                 // set address family to 'internet'
    serv_addr.sin_port = htons(port);               // convert host port to network port
    serv_addr.sin_addr.s_addr = INADDR_ANY;         // set server IP to symbolic
    
    /* bind socket */
    if( bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0 ){
        error("Coulnd't bind socket");}
    
    /* go online */
    listen(sock, 50);    // 5 is the standard maximum for waiting socket clients
    online = 1;
    printf("\n------------------------------\n    Listening on port %d\n       EXIT WITH CTRL+C       \n------------------------------\r\n",port);
}

/* accept socket connections, spin up thread and hand off session to thread */
void accept_connections()
{
    cli_addr_len = sizeof(cli_addr);
    while(online){
        int id = add_client();
        if( id != NUM_THREADS ){
            bzero(&cli_addr[id], sizeof(struct sockaddr_in));
            connections[id] = accept(sock,
                                     (struct sockaddr*) &cli_addr[id],
                                     (unsigned int *)&cli_addr_len);
            if ( connections[id] < 0 ){ error("Error accepting client.\n"); }
            else{
                setsockopt(connections[id], SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,sizeof(struct timeval));
            }
            if ( pthread_create(&threads[id], NULL, serve, &session[id]) != 0 ){
                error("Couldn't create thread");
            }
        }
    }
}

/* thread workload -- respond to client requests */
void* serve(void* arg)
{
    /* allocate resources */
    int id = *((int*)arg);                  // local copy of id makes following code cleaner
    buffers[id] = malloc(BUFFER_SIZE);      // create buffer for session
    //buffers[id+NUM_THREADS] = malloc(BUFFER_SIZE);
    
    /* serving client */
    while( is_online() )
    {
        bzero(buffers[id], BUFFER_SIZE);                                  // clean buffer
        //bzero(buffers[id+NUM_THREADS], BUFFER_SIZE);
        
        int n = (int)read(connections[id],buffers[id],BUFFER_SIZE);       // read request from socket
        
        // if there's nothing to read
        if( n <= 0 ){
            printf("\nNothing to read.");
            break;
        }
        
        else{
            if( connections[id+NUM_THREADS] == 0 ){
                connect_host(id);
            }
            relay_request(id);     // relay request to remote host
            printf("\n\nRelayed:\n\n%s",buffers[id]);
            if( relay_response(id) ){
                printf("\nClose signal.");
                break;
            }          // send HTTP payload
            
            /* NO CONNECTIONS ARE PERSISTANT--if response was sent, close connection */
        }
    }
    
    /* ending session */
    close(connections[id+NUM_THREADS]);
    close(connections[id]);   // close connection
    //free(buffers[id+NUM_THREADS]);              // deallocate buffer
    free(buffers[id]);              // deallocate buffer
    printf("\n\nExiting client %d...\n\n",session[id]);
    sub_client(id);                 // deallocate client ID
    pthread_detach(threads[id]);    // detach thread to deallocate resources
    return NULL;
}

void grab_host(int id)
{
    /* save host name from buffer */
    char* end = strnstr(buffers[id], "Host: ", BUFFER_SIZE);    // find beginning of host name
    if( end == NULL ){ error("No host specified in request. Cannot relay."); }
    char* start = end + 6;                              // move past "Host: "
    end = strnstr(start, "\r\n", 256);                  // find end of line
    int total_host_len = (int)(end - start);
    char* port_specified = strnstr(start, ":", total_host_len);
    if( port_specified != NULL ){
        end = port_specified;
    }
    int host_len = (int)(end - start);                  // find length of host name
    char host[host_len];
    strncpy(host, start, host_len);
    host[host_len] = '\0';
}

int grab_port(int id)
{
    /* save host name from buffer */
    char* start = strnstr(buffers[id], "Host: ", BUFFER_SIZE);// find beginning of port
    start += 6;
    char* end_host = strnstr(start, "\r\n", BUFFER_SIZE);
    int total_len = (int)(end_host - start);
    start = strnstr(start, ":", total_len);
    if( start == NULL ){
        return 80;
    }
    char* end = strnstr(start, "\r\n", BUFFER_SIZE-128);                  // find end of host name
    int port_len = (int)(end - start);                  // find length of host name
    char port_[port_len+1];
    strncpy(port_, start, port_len);
    port_[port_len] = '\0';
    return atoi(port_);
}

int contains_close( int id )
{
    char* close = strnstr(buffers[id], "\r\nConnection: close\r\n", BUFFER_SIZE);
    if( close == NULL ){ return 0; }
    printf("\n\nContains close...\n\n");
    return 1;
}

void connect_host(int id)
{
    //bzero(&remote_host[id], sizeof(struct hostent));
    //bzero(&host_addr[id], sizeof(struct sockaddr_in));

    /* save host name from buffer */
    char* end = strnstr(buffers[id], "Host: ", BUFFER_SIZE);    // find beginning of host name
    if( end == NULL ){ error("No host specified in request. Cannot relay."); }
    char* start = end + 6;                              // move past "Host: "
    end = strnstr(start, "\r\n", 256);                  // find end of line
    int total_len = (int)(end - start);
    char* port_specified = strnstr(start, ":", total_len);
    if( port_specified != NULL ){
        end = port_specified;
    }
    int host_len = (int)(end - start);                  // find length of host name
    char host[host_len];
    strncpy(host, start, host_len);
    host[host_len] = '\0';
    
    /* save port number from buffer */
    start = strnstr(buffers[id], "Host: ", BUFFER_SIZE);// find beginning of host
    start += 6;
    char* end_host = strnstr(start, "\r\n", BUFFER_SIZE);
    total_len = (int)(end_host - start);
    start = strnstr(start, ":", total_len);
    char port_num[16];
    if( start == NULL ){
        strcpy(port_num, "80");
        port_num[2] = '\0';
    }
    else{
        start += 1;
        end = strnstr(start, "\r\n", BUFFER_SIZE);                  // find end of host name
        int port_len = (int)(end - start);                  // find length of host name
        strncpy(port_num, start, port_len);
        port_num[port_len] = '\0';
    }
    
    struct addrinfo hints, *res;
    
    // first, load up address structs with getaddrinfo():
    
    bzero(&hints, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    getaddrinfo(host, port_num, &hints, &res);
    
    // make a socket:
    
    connections[id+NUM_THREADS] = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    setsockopt(connections[id+NUM_THREADS], SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,sizeof(struct timeval));
    // connect!
    
    if( connect(connections[id+NUM_THREADS], res->ai_addr, res->ai_addrlen) == -1){
        error("connect");
    }
    struct sockaddr_in *addr;
    addr = (struct sockaddr_in *)res->ai_addr;
    printf("\nConnected to: %s :: %d\n",inet_ntoa((struct in_addr)addr->sin_addr),ntohs(addr->sin_port));
}


/* print generic response header to buffer */
/*
char* header_helper(int id, int status_code, char* status_msg, char* content_type, int content_length, int filename_length)
{
    char* header = buffers[id] + filename_length + 1;     // write header after file path in the buffer
    snprintf(header,
             120,   // limit on header length to avoid overflow in small buffers
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "User-Agent: %s\r\n"
             "Connection: close\r\n"
             "\r\n", status_code, status_msg, content_type, content_length, user_agent);
    return header;
}*/

/* print status 301 header in buffer for redirect functionality */
char* redirect_helper(int id, char* url)
{
    char* header = buffers[id] + strlen(url) + 1;     // write header after file path in the buffer
    snprintf(header,
             120,   // limit on header length to avoid overflow in small buffers
             "HTTP/1.1 301 Moved Permanently\r\n"
             "Location: %s\r\n"
             "\r\n", url);
    return header;
}

/* send header to client */
void relay_request( int id )
{
    char* header = buffers[id];
    if( header != NULL ){
        int n = (int)strlen(header);
        printf("\nLength of request: %d\n",n);
        void* pos = header;
        while (n > 0) {
            int bytes_written = (int)write(connections[id+NUM_THREADS], pos, n);
            if (bytes_written <= 0) { error("Couldn't relay request."); }
            n -= bytes_written;
            pos += bytes_written;
        }
    }
}

/* Send input_file to client, optionally filling dynamic template data */
int relay_response(int id)
{
    int close = 0;
    
    /* send file to client */
    int bytes_recv = 1;
    int bytes_writ = 0;
    printf("\n\nResponse = ");
    
    while ( bytes_recv ) {
        bzero(buffers[id], BUFFER_SIZE);
        bytes_recv = 0;
        
        bytes_recv = (int)read(connections[id+NUM_THREADS], buffers[id], BUFFER_SIZE);       // Read data into buffer.
        
        if( bytes_recv < 0 ){ break; }
                                        // Exit if nothing to read.
        if ( contains_close(id)){ close = 1; }
        
        bytes_writ = (int)write(connections[id], buffers[id], bytes_recv);
        if( bytes_writ != bytes_recv ){ error("read/write mismatch"); }
        
        printf("%d + ",bytes_recv);
    }
    fflush(stdout);
    return close;
}


/* builds URL of requested file based on packet's request line and "Host:" value */
char* url_build(int id, char* packet)
{
    char* start = strnstr(packet, "/go/", 100); // find beginning of hostname
    start += 4;                                  // move past "/go/"
    char* end = strnstr(start, " ", 100);       // find end of hostname
    int len = (int)(end - start);               // find length of filename
    char hostname[len+1];                       // allocate storage
    strncpy(hostname, start, len);              // copy hostname, buffer about to be wiped
    hostname[len] = '\0';                       // add null (strncpy doesn't)
    bzero(buffers[id], BUFFER_SIZE);            // clear buffer
    int url_components = 7 + 4 + 4 + 1;         // "http://" + "www." + ".com\0"
    snprintf(buffers[id], len+url_components, "http://www.%s.com",hostname);
    return buffers[id];
}

/* allocates client id */
int add_client()
{
    int e = 0;
    pthread_mutex_lock(&id_mutex);
    while( (session[e] != NUM_THREADS) && (e < NUM_THREADS) ){    // find available id for client
        e++;
    }
    if( e < NUM_THREADS ){ session[e] = e; }    // if id allocated, mark as used
    pthread_mutex_unlock(&id_mutex);
    //if( e == NUM_THREADS ){
        //return NUM_THREADS;
    //}
    return e;
}

/* deallocates client id */
void sub_client(int id)
{
    pthread_mutex_lock(&id_mutex);
    session[id] = NUM_THREADS;        // mark client ID as available
    connections[id+NUM_THREADS] = 0;
    pthread_mutex_unlock(&id_mutex);
}

/* deallocates client id */
int is_online()
{
    int x;
    pthread_mutex_lock(&online_mutex);
    x = online;
    pthread_mutex_unlock(&online_mutex);
    return x;
}

int is_mobile()
{
    int x;
    pthread_mutex_lock(&mobile_mutex);
    x = mobile;
    pthread_mutex_unlock(&mobile_mutex);
    return x;
}

/* deallocates client id */
void disable_mobile()
{
    pthread_mutex_lock(&mobile_mutex);
    mobile = 0;
    pthread_mutex_unlock(&mobile_mutex);
}

/* deallocates client id */
void enable_mobile()
{
    pthread_mutex_lock(&mobile_mutex);
    mobile = 1;
    pthread_mutex_unlock(&mobile_mutex);
}

/* deallocates client id */
void enable_redirect(char* host)
{
    pthread_mutex_lock(&redirect_mutex);
    redirect = 1;
    redirect_host = malloc(strlen(host));
    strcpy(redirect_host, host);
    pthread_mutex_unlock(&redirect_mutex);
}

/* deallocates client id */
void disable_redirect()
{
    pthread_mutex_lock(&redirect_mutex);
    redirect = 0;
    if( redirect_host != NULL ){ free(redirect_host); }
    pthread_mutex_unlock(&redirect_mutex);
}

/* deallocates client id */
void go_offline()
{
    pthread_mutex_lock(&online_mutex);
    online = 0;
    pthread_mutex_unlock(&online_mutex);
}

/* Stop & join threads, close sockets, free malloc's, & exit. */
void cleanup_and_exit()
{
    printf("\nCleaning up...");
    go_offline();
    for( int i = 0; i < NUM_THREADS; i++ ){
        pthread_join(threads[i], NULL);
    }
    printf("\nthreads terminated...");
    for( int i = 0; i < NUM_THREADS; i++ ){
        shutdown(connections[i], 2);
    }
    shutdown(sock, 2);
    printf("\nsockets/connections closed...");
    //free(dir);
    if( mobile_ids != NULL){ free(mobile_ids); }
    printf("\ngoodbye!\n");
    exit(0);
}

/* error handler */
void error(char *msg)
{
    perror(msg);
    exit(1);
}
