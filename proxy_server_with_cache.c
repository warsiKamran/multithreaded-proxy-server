#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_ELEMENT_SIZE 10*(1<<10)
#define MAX_SIZE 200*(1<<20)

typedef struct cacheElement cacheElement;

//defining elements which we are going to store in lru cache
//elements will be in form of linked list , therefore using next pointer
struct cacheElement{
    char *data; 
    int len;
    char *url;
    time_t lru_time_track;
    cacheElement *next;
};

cacheElement *findElement(char *url);
int addCacheElement(char *data, int size, char *url);
void removeCacheElement();

int portNumber = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;          //for sem_wait() and sem_signal(), multiple threads can work at once.
pthread_mutex_t lock;    // since there is one lru cache and multiple threads will access it due to which race condition can occur, therefore using lock

cacheElement* head;
int cacheSize;

int connectRemoteServer(char* host_addr, int portNumber){

    //connecting with end server and opening socket for it
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(remoteSocket < 0){
        printf("error in creating socket\n");
        return -1;
    }

    //gethostbyname retrieves the host information for the provided hostname (host_addr).
    // It returns a pointer to a hostent structure containing information about the host, including its IP addresses.
    struct hostent* host = gethostbyname(host_addr);

    if(host == NULL){
        fprintf(stderr, "No such host exists\n");
        return -1;
    }

    //Initializes a sockaddr_in structure named server_addr, which will hold the address information of the server to connect to.
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portNumber);

    //We are copying the resolved IP address from the host (obtained via gethostbyname) into the server_addr structure to specify the destination for the socket connection. This step is essential because the connect function requires a full address (both IP and port) to establish a successful connection to the remote server.

    //Copies the first IP address from the h_addr_list (array of addresses) into server_addr.sin_addr.s_addr.
    //This sets the destination IP address for the socket connection.
    bcopy((char *)host->h_addr_list[0], (char *)&server_addr.sin_addr.s_addr, host->h_length);

    //Attempts to connect the socket (remoteSocket) to the server specified in server_addr.
    if(connect(remoteSocket, (struct sockaddr*)&server_addr, (size_t)sizeof(server_addr) < 0)){
        fprintf(stderr, "Error in connecting\n");
        return -1;
    }

    return remoteSocket;
}

int handle_request(int client_socketId,  struct ParsedRequest* request, char* tempReq){

    //Allocates a buffer buf with a size of MAX_BYTES to store the HTTP request message that will be forwarded to the remote server.
    char* buf = (char*)malloc(sizeof(char)*MAX_BYTES);

    //The request is constructed with GET as the HTTP method (only GET requests are supported here).
    strcpy(buf, "GET");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    //This line ensures that the Connection header is set to close, instructing the server to close the connection after responding.
    if(ParsedHeader_set(request, "Connection", "close") < 0){
        printf("Set Header key is not working\n");
    }

    //Checks if the Host header is missing using ParsedHeader_get.
    //If absent, it sets the Host header to the value of request->host.
    if(ParsedHeader_get(request, "Host") == NULL){

        if(ParsedHeader_set(request, "Host", request->host) < 0){
            printf("Set Host header key is not working\n");
        }
    }

    //ParsedRequest_unparse_headers writes all the headers from request into the buf after the request line, starting at buf + len.
    //This step finalizes the full HTTP request by adding all headers to the buffer, making it ready for transmission.
    if(ParsedRequest_unparse_headers(request, buf+len, (size_t)MAX_BYTES - len < 0)){
        printf("unparse failed");
    }

    //default http port is 80 of end server not proxy server
    int serverPort = 80;

    if(request->port != NULL){
        serverPort = atoi(request->port);
    }

    //Calls connectRemoteServer to open a socket and connect to the remote server(end server).
    // This establishes a network connection to request->host at serverPort, allowing the server to forward the client’s request to the remote host.
    int remoteSocketId = connectRemoteServer(request->host, serverPort);

    if(remoteSocketId < 0){
        return -1;
    }

    //Here, the code sends the HTTP request stored in buf to the remote server using the send function.
    //After sending, it clears the buf to prepare it for receiving data.
    int bytesSend = send(remoteSocketId, buf, strlen(buf), 0);
    bzero(buf, MAX_BYTES);

    //This line receives the server's response into buf.
    //It initializes tempBuffer to store the data received, allocating memory for it.
    bytesSend = recv(remoteSocketId, buf, MAX_BYTES-1, 0);
    char* tempBuffer = (char*)malloc(sizeof(char)*MAX_BYTES);

    int tempBufferSize = MAX_BYTES;
    int tempBufferIdx = 0;

    // A loop is initiated to process the data received from the remote server.
    // The send function sends the data received from the remote server to the original client connected through client_socketId.
    // The response data is also stored in tempBuffer.
    while(bytesSend > 0){

        bytesSend = send(client_socketId, buf, bytesSend, 0);

        for(int i=0; i<bytesSend/sizeof(char); i++){
            tempBuffer[tempBufferIdx] = buf[i];
            tempBufferIdx++;
        }

        //After each iteration of sending data, the code dynamically increases the size of tempBuffer to accommodate more incoming data by using realloc.
        //This is necessary since the response from the server could be larger than the initial buffer size.
        tempBufferSize += MAX_BYTES;
        tempBuffer = (char*)realloc(tempBuffer, tempBufferSize);

        if(bytesSend < 0){
            perror("Error in sending data to client\n");
            break;
        }

        bzero(buf, MAX_BYTES);
        bytesSend = recv(remoteSocketId, buf, MAX_BYTES-1, 0);
    }

    tempBuffer[tempBufferIdx] = '\0';

    free(buf);
    addCacheElement(tempBuffer, strlen(tempBuffer), tempReq);
    free(tempBuffer);
    close(remoteSocketId);

    return 0;
}   

int checkHTTPversion(char* msg){

    int version = -1;

    if(strncmp(msg, "HTTP/1.1", 8) == 0){
        version = 1;
    }

    else if(strncmp(msg, "HTTP/1.0", 8) == 0){
        version = 1;
    }

    else{
        version = -1;
    }

    return version;
}

int sendErrorMessage(int socket, int status_code){

    char str[1024];
	char currentTime[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(status_code)
	{
		case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
				  printf("400 Bad Request\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
				  printf("403 Forbidden\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
				  printf("404 Not Found\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
				  //printf("500 Internal Server Error\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
				  printf("501 Not Implemented\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, str, strlen(str), 0);
				  break;

		default:  return -1;

	}
	return 1;
}

//the socketNew parameter is a pointer that holds the value of the client's socket descriptor. This socket descriptor (socketNew) is passed to each thread when a new client connection is accepted
void *thread_fn(void *socketNew){

    sem_wait(&semaphore);
    int p;

    sem_getvalue(&semaphore, &p);
    printf("semaphore value is: %d\n", p);


    //The function casts the void* argument to an int*, allowing it to retrieve the socket file descriptor that was passed when the thread was created.
    int *t = (int*) socketNew;
    int socket = *t;

    int bytes_send_client, len;

    //Allocates memory for a buffer to hold incoming data from the client and initializes it to zero.
    char* buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);

    //The received data is stored in buffer, and bytes_send_client holds the number of bytes received from the client.
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);

    //This loop continues to receive data until the end of the HTTP header is reached (indicated by \r\n\r\n).
    while(bytes_send_client > 0){

        len = strlen(buffer);

        if(strstr(buffer, "\r\n\r\n") == NULL){
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES-len, 0);
        }
        else{
            break;
        }
    }   

    //now request has came to proxy server and we want to search it in cache , therefore making a temp copy for it
    char* tempReq = (char*)malloc(strlen(buffer)*sizeof(char)+1);

    for(int i=0; i<strlen(buffer); i++){
        tempReq[i] = buffer[i];
    }   

    struct cacheElement* temp = findElement(tempReq);

    //f temp (the cache element) exists, the function prepares to send it to the client.
    //Using a loop, it copies MAX_BYTES from temp->data into the response buffer until all data is sent.
    // bzero clears response for each iteration to avoid residual data from previous loops.
    // send() transmits response to the client.
    if(temp != NULL){

        int size = temp->len/sizeof(char);
        int pos = 0;

        char response[MAX_BYTES];

        while(pos < size){

            bzero(response, MAX_BYTES);

            for(int i=0; i<MAX_BYTES; i++){
                response[i] = temp->data[i];
                pos++;
            }

            send(socket, response, MAX_BYTES, 0);
        }

        printf("Data retrieved from cache\n");
        printf("%s\n\n", response);
    }
 
    //Handle Non-Cached Client Requests
    //ParsedRequest_create() initializes a ParsedRequest structure to hold parsed request details.
    //ParsedRequest_parse() attempts to parse buffer into the request structure.
    else if(bytes_send_client > 0){

        len = strlen(buffer);
        struct ParsedRequest* request = ParsedRequest_create();

        if(ParsedRequest_parse(request, buffer, len) < 0){
            printf("parsing failed\n");
        }

        //a comparison checks if the method is "GET".
        else{

            bzero(buffer, MAX_BYTES);

            if(!strcmp(request->method, "GET")){

                if(request->host && request->path && checkHTTPversion(request->version) == 1){

                    bytes_send_client = handle_request(socket, request, tempReq);

                    if(bytes_send_client == -1){
                        sendErrorMessage(socket, 500);
                    }
                }

                else{
                    sendErrorMessage(socket, 500);
                }
            }

            else{
                printf("This code doesn't support any method apart from GET\n");
            }
        }

        ParsedRequest_destroy(request);
    }

    else if(bytes_send_client == 0){
        printf("client is disconnected\n");
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);

    printf("semaphore post value is %d\n", p);
    free(tempReq);

    return NULL;
}   

//struct sockaddr is a generic structure used to store addresses for different types of network communications (like TCP, UDP, etc.) in the C programming language.
//It is used with functions like bind(), connect(), and accept() in network programming to handle IP addresses and port numbers.

//argc stands for "argument count".
//It is an integer that represents the number of command-line arguments passed to the program, including the program name itself.
//For example, if you run the command ./proxy 8080, argc will be 2 (one for ./proxy and one for 8080).

//argv stands for "argument vector".
//It is an array of C-strings (character pointers) that holds each command-line argument as a string.
//argv[0] contains the program name ("./proxy" in the example above), and argv[1] would contain "8080".

int main(int argc, char* argv[]){

    int client_socketId, clientLen;
    struct sockaddr_in server_addr, client_addr;

    //initialising semaphore and lock
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    if(argc == 2){
        portNumber = atoi(argv[1]);
    } 
    else{
        printf("Few arguments\n");
        exit(1);  
    }

    printf("proxy server started at %d\n", portNumber);

    //opening socket of proxy server

    // The socket() function is used to create a new socket, which is an endpoint for communication between two machines over a network.

    // AF_INET:
    // This argument specifies the address family.
    // AF_INET stands for IPv4 (Internet Protocol version 4), meaning that the socket will use IPv4 addresses.

    // SOCK_STREAM:
    // This argument specifies the type of socket.
    // SOCK_STREAM means it will be a TCP socket, which is a connection-oriented protocol. TCP guarantees reliable data transfer, ensuring that data packets are delivered in order and without corruption.

    // 0:
    // This specifies the protocol.
    // Usually, you can pass 0 to let the system select the default protocol for the given socket type. For SOCK_STREAM, the default protocol is TCP, so 0 is valid here.
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if(proxy_socketId < 0){
        perror("socket creation failed\n");
        exit(1);
    }

    // The setsockopt() function is used to set options for sockets. It allows you to change the behavior of the socket, such as allowing it to reuse an address, control timeout behavior, or set buffer sizes.

    // SOL_SOCKET: jo bhi options diye hai use socket lvl parr set karke aa jao
    // SO_REUSEADDR: use the same addres , if google.com comes then return it from same socket rather than returning that server is busy
    // (const char*)&reuse: This is a pointer to the value of the option you're setting. In this case, it's a pointer to reuse, which is 1, meaning "enable address reuse."
    // sizeof(reuse): This specifies the size of the option value (in bytes).
    // basically if same req is coming we will return it from same socket only
    int reuse = 1;

    if(setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0){
        perror("setSockOpt failed\n");
    }

    //clearing garbage values
    bzero((char*) &server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;           //Specifies that the address is for IPv4.
    server_addr.sin_port = htons(portNumber);  //Sets the port to 8080 and converts it to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY;  //Sets the IP address

    //The bind() function is essential for making a socket listen on a specific IP address and port.
    if(bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr) < 0)){
        perror("Port is not available\n");
        exit(1);
    }

    printf("Binding on port%d\n", portNumber);

    //setting up a server to accept incoming connection requests. by this our proxy server will start listening.
    int listenStatus = listen(proxy_socketId, MAX_CLIENTS);

    if(listenStatus < 0){
        perror("error in listening\n");
        exit(1);
    }

    int i = 0;
    //Declares an array to store the socket IDs of connected clients, with a maximum limit defined by MAX_CLIENTS.
    int connected_socketId[MAX_CLIENTS];


    while(1){

        bzero((char *)&client_addr, sizeof(client_addr));
        clientLen = sizeof(client_addr);

        //This line waits for an incoming connection on the proxy_socketId. When a client connects, it creates a new socket (client_socketId) for that connection and fills in client_addr with the client's address information.
        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, (socklen_t*)&clientLen);

        if(client_socketId < 0){
            printf("Not able to connect\n");
            exit(1);
        }
        else{
            connected_socketId[i] = client_socketId;
        }

        // Retrieve and print client's IP address from client_addr and port number
        //creates pointer that points to client_addr
        struct sockaddr_in * client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        //This line declares a character array str to hold the string representation of the IP address.
        char str[INET_ADDRSTRLEN];

        //This converts the binary IP address stored in ip_addr to a string format and stores it in the str array.
        //INET_ADDRSTRLEN is a constant defined in <netinet/in.h> that represents the size of a buffer needed to hold an IPv4 address in its string representation
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("client's port number is %d and ip is %s\n", ntohs(client_addr.sin_port), str);

        //now all connections are accepted
        // giving thread to the opened socket
        //&tid[i]: A pointer to the thread identifier (pthread_t). This is where the thread ID will be stored. 
        //This is the argument that will be passed to the thread_fn. It’s cast to void* because threads can only accept a single pointer as an argument. Here, it’s passing the socket ID of the connected client so that the thread can use it to communicate with the client.
        pthread_create(&tid[i], NULL, thread_fn, (void*)&connected_socketId[i]);
        i++;
    }   

    close(proxy_socketId);
    return 0;
}       

cacheElement* findElement(char* url){

    cacheElement* site = NULL;
    int tempLockValue = pthread_mutex_lock(&lock);

    printf("Remove cache lock acquired %d\n", tempLockValue);

    if(head != NULL){

        site = head;

        while(site != NULL){

            if(!strcmp(site->url, url)){

                printf("LRU time track before %ld", site->lru_time_track);
                printf("\n url found\n");

                //update time
                site->lru_time_track = time(NULL);

                printf("LRU time track after %ld", site->lru_time_track);
                break;
            }

            site = site->next;
        }
    }  

    else{
        printf("url not found\n");
    }

    tempLockValue = pthread_mutex_unlock(&lock);
    printf("Lock has been unlocked\n");

    return site;
}

void removeCacheElement(){

    cacheElement* p;
    cacheElement* q;
    cacheElement* temp;

    int tempLockValue = pthread_mutex_lock(&lock);
    printf("Lock is acquired\n");

    if(head != NULL){

        for(q=head, p=head, temp=head; q->next != NULL; q=q->next){

            if(q->next->lru_time_track < temp->lru_time_track){
                temp = q->next;
                p = q;
            }
        }

        if(temp == head){
            head = head->next;
        }

        else{
            p->next = temp->next;
        }

        cacheSize = cacheSize - (temp->len) - sizeof(cacheElement) - strlen(temp->url) - 1;

        free(temp->data);
        free(temp->url);
        free(temp);
    }

    tempLockValue = pthread_mutex_unlock(&lock);
    printf("Cache lock removed\n");

    return;
}

int addCacheElement(char* data, int size, char* url){

    cacheElement* site = NULL;
    int tempLockValue = pthread_mutex_lock(&lock);

    int totalSize = size + 1 + strlen(url) + sizeof(cacheElement);

    if(totalSize > MAX_ELEMENT_SIZE){

        tempLockValue = pthread_mutex_unlock(&lock);
        printf("Add cache lock is unlocked and element cannot be added");

        return 0;
    }

    else{

        while(cacheSize + totalSize > MAX_SIZE){
            //means our cache is full so remove one element
            removeCacheElement();
        }

        cacheElement* element = (cacheElement*)malloc(sizeof(cacheElement));

        element->data = (char*)malloc(size+1);
        strcpy(element->data, data);

        element->url = (char*)malloc(1 + (strlen(url) * sizeof(char)));
        strcpy(element->url, url);

        element->lru_time_track = time(NULL);
        element->next = head;
        element->len  = size;

        head = element;
        cacheSize += totalSize;

        tempLockValue = pthread_mutex_unlock(&lock);
        printf("Add cache lock is unlocked");  

        return 1;
    }

    return 0;
}

