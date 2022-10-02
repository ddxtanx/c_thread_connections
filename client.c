#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>

int main(int argc, char** argv){
    int socketfd = socket(AF_INET, SOCK_STREAM, 0);

    struct in_addr local_addr;

    const char* local_ip = "127.0.0.1";

    int ip_cast = inet_aton(local_ip, &local_addr);
    if(ip_cast == 0){
        perror("ip failed to translate");
        return 1;
    }

    struct sockaddr_in sock_connect;

    sock_connect.sin_family = AF_INET;
    sock_connect.sin_port = 65535;
    sock_connect.sin_addr = local_addr;

    int connectstatus = connect(socketfd, (const struct sockaddr*)&sock_connect, sizeof(struct sockaddr_in));

    int len = 9;
    len = htons(len);
    char* msg = malloc(sizeof(int) + 9);

    *((int*)msg) = len;
    msg[4] = 'n';
    strcpy(msg+5, "garrett");
    msg[12] = '\0';
    printf("Num bytes: %d\n", *((int*)msg));
    printf("Message: %s\n", msg+sizeof(int));

    printf("Writing message \n");
    write(socketfd, msg, 13);
    printf("Wrote message \n");

    int num_bytes;
    printf("reading response\n");
    read(socketfd, (void*)&num_bytes, 4);

    printf("Response len %d\n", ntohs(num_bytes));

    if(num_bytes == 0){
        printf("Error on connection\n");
        close(socketfd);
        return 1;
    }

    num_bytes = ntohs(num_bytes);
    
    char* resp = malloc(num_bytes);

    read(socketfd, (void*)resp, num_bytes);

    size_t client_id = ntohs(*(int*)resp);

    printf("Response: %zu\n", client_id);

    char* arg_msg;
    size_t arg_len;
    if(argc == 3){
        arg_msg = argv[2];
        arg_len = strlen(arg_msg);
    } else{
        arg_msg = malloc(8);
        strcpy(arg_msg, "default");
        arg_len = 8;
    }

    printf("Atttempting to send message %s\n", arg_msg);

    char* arg_send = malloc(2*4 + arg_len);
    *((int*)arg_send) = htons(4 + arg_len);
    *((int*)arg_send + 1) = htons(client_id);
    strcpy(arg_send + 8, arg_msg);

    printf("Sending message of len %zu\n", 2*4+arg_len);
    write(socketfd, arg_send, 2*4 + arg_len);
    free(arg_msg);
    free(arg_send);
    printf("Sent\n");
    int new_msg_len;
    while(1){
        printf("Recieving message from server\n");
        recv(socketfd, &new_msg_len, 4, MSG_WAITALL);
        printf("Recieved message \n");

        if(new_msg_len == 0){
            break;
        }

        new_msg_len = ntohs(new_msg_len);
        printf("Of length %d\n", new_msg_len);

        char* server_msg = malloc(new_msg_len);
        recv(socketfd, server_msg, new_msg_len, MSG_WAITALL);
        printf("%s\n", server_msg);
        free(server_msg);
    }
    


    char close_con[6];

    *((int*)close_con) = htons(1);
    close_con[4] = 'c';
    close_con[5] = 0;

    write(socketfd, (void*)close_con, 6);
    close(socketfd);

    free(msg);
    free(resp);
}