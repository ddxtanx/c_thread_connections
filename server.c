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
#include <signal.h>

size_t MAX_CONNECTIONS = 5;
size_t LISTEN_THREADS = 5;
bool debug = true;

typedef struct _server_state{
    int server_socket_fd; // File descriptor for servers socket
    int* client_socket_fds; // List of file descriptors for client connection
    char** client_names; // List of names for clients
    size_t current_connections; // Current number of connections

    char* message; // message to transmit
    int originator; // Client who sent message
    int messages_sent; // number of messages sent
    bool* has_sent; // Array to keep track of messages sent

    pthread_mutex_t msg_lock; // lock to prevent multiple messages being written
    pthread_cond_t msg_cond;
    pthread_cond_t all_sent;
} server_state;

server_state* s = NULL;

typedef struct _sender_state{
    server_state* s_state;

    int connection_handled;
} sender_state;

void end_connection(int client_fd){
    char zero = 0;
    write(client_fd, &zero, 1);
    close(client_fd);
    // If client cannot connect, sends null message and closes connection
}

void interrupt(int signum){
    if(s != NULL){
        for(int i = 0; i < MAX_CONNECTIONS; i++){
            if(s -> client_names[i] != NULL){
                end_connection(s -> client_socket_fds[i]);
            }
        }
    }
    signal(SIGINT, SIG_DFL);
    raise(SIGINT);
}

void* handle_connection(void* server_state_ptr){
    server_state* state = (server_state*)server_state_ptr;

    int client_fd = accept(state -> server_socket_fd, NULL, NULL);
    bool new_connection = true;
    while(client_fd != -1){
        if(debug) printf("Client connected\n");

        if(new_connection && state->current_connections == MAX_CONNECTIONS){
            end_connection(client_fd);
            client_fd = accept(state -> server_socket_fd, NULL, NULL);
            continue;
            // If client cannot connect, sends null message and closes connection
        }
        new_connection = false;


        int msg_len;

        if(debug) printf("Reading from fd\n");
        int init_read = recv(client_fd, (void*)&msg_len, 4, MSG_WAITALL);
        if(init_read != 4){
            end_connection(client_fd);
            state -> current_connections --;
            new_connection = true;
            client_fd = accept(state -> server_socket_fd, NULL, NULL);
            continue;
        }

        msg_len = ntohs(msg_len);

        if(debug) printf("Message is %d bytes long\n", msg_len);

        char* msg = malloc(msg_len);

        size_t recv_read = recv(client_fd, msg, msg_len, MSG_WAITALL);
        if(recv_read != msg_len){
            free(msg);
            close(client_fd);
            state -> current_connections --;
            new_connection = true;
            client_fd = accept(state -> server_socket_fd, NULL, NULL);
            continue;
        }

        // First byte will be n if it is name connection
        if(msg[0] == 'n'){
            char* name = malloc(msg_len-1);
            printf("Name len is %d\n", msg_len - 1);
            printf("Message is %s\n", msg);
            memcpy(name, msg+1, msg_len - 1);
            if(debug) printf("Name of client is %s\n", name);

            bool found_spot = false;
            int client_id = -1;
            for(int i = 0; i < MAX_CONNECTIONS; i++){
                if(state->client_names[i] == NULL){
                    client_id = i;
                    state->client_names[i] = name;
                    state->client_socket_fds[i] = client_fd;
                    found_spot = true;
                    break;
                }
            }

            printf("%s's client id is %d\n", name, client_id);

            if(!found_spot){
                free(name);
                free(msg);
                end_connection(client_fd);
                continue;
            }

            char resp[8];
            *((int*)resp) = htons(4);
            *((int*)resp + 1) = htons(client_id);

            if(debug) printf("Sending response\n");
            state -> current_connections ++;
            write(client_fd, resp, 8);
            if(debug) printf("Send response\n");

            char* template_str = "Client  has connected.";
            char* send_str = malloc(strlen(template_str) + strlen(name) + 1);
            sprintf(send_str, "Client %s has connected.", name);

            pthread_mutex_lock(&state ->msg_lock);
            while(state -> message != NULL){
                pthread_cond_wait(&state -> all_sent, &state -> msg_lock);
            }
            // Send message like Client [name] has connected.
            printf("Notifying that %s has connected.\n", name);
            state -> message = send_str;
            state -> originator = client_id;
            pthread_cond_broadcast(&state -> msg_cond);
            pthread_mutex_unlock(&state -> msg_lock);
        } else if(strncmp(msg, "c", 1) == 0){ // Close connection
            int client_id;

            client_id = ntohs(*(int*)(msg + 1));
            char* client_name = state -> client_names[client_id];

            printf("Client %d closes\n", client_id);

            char* template = "Client  has disconnected.";
            char* discon_message = malloc(strlen(client_name) + strlen(template) + 1);
            sprintf(discon_message, "Client %s has disconnected.", client_name);

            printf("Created discon str: %s\n", discon_message);

            if(state->client_names[client_id] != NULL){
                free(state->client_names[client_id]);
            }

            state -> client_names[client_id] = NULL;

            state->client_socket_fds[client_id] = 0;
            state -> current_connections --;
            close(client_fd);

            printf("Freed data\n");

            pthread_mutex_lock(&state -> msg_lock);
            while(state -> message != NULL){
                pthread_cond_wait(&state -> all_sent, &state -> msg_lock);
            }
            printf("Setting message: %s from %d\n", discon_message, client_id);
            state -> message = discon_message;
            state -> originator = client_id;
            pthread_cond_broadcast(&state -> msg_cond);
            pthread_mutex_unlock(&state -> msg_lock);
            client_fd = accept(state -> server_socket_fd, NULL, NULL);
        } else if(msg[0] == 'm'){ // Recieving Message, first byte is client id
            int client_id;
            printf("Determining client id\n");

            client_id = ntohs(*((int*)(msg + 1)));

            char* client_name = state->client_names[client_id];

             printf("Client id is %d, name is %s\n", client_id, client_name);
            char* client_message = malloc(msg_len - 5);
            strcpy(client_message, msg + 5);
            

            printf("Recieved message from %s: %s\n", client_name, client_message);

            pthread_mutex_lock(&state -> msg_lock);
            while(state -> message != NULL){
                pthread_cond_wait(&state -> all_sent, &state -> msg_lock);
            }
            state -> message = client_message;
            state -> originator = client_id;
            pthread_cond_broadcast(&state -> msg_cond);
            pthread_mutex_unlock(&state -> msg_lock);
        } else{
            printf("%s is invalid\n", msg);
        }

        free(msg);
    }
    return NULL;
}

void* send_message(void* sender_state_ptr){
    signal(SIGINT, interrupt);
    sender_state* send_state = sender_state_ptr;

    int client_responsible_id = send_state -> connection_handled;

    while(1){
        pthread_mutex_lock(&send_state -> s_state -> msg_lock);
        while(
            send_state -> s_state -> client_names[client_responsible_id] == NULL || 
            send_state -> s_state -> message == NULL || 
            send_state -> s_state -> has_sent[client_responsible_id] ||
            send_state -> s_state -> originator == client_responsible_id
        ){
            pthread_cond_wait(&send_state -> s_state ->msg_cond, &send_state -> s_state -> msg_lock);
            if(
                send_state -> s_state -> current_connections == 0 ||
                (send_state -> s_state -> client_names[client_responsible_id] != NULL &&
                send_state -> s_state -> originator == client_responsible_id && 
                send_state -> s_state -> current_connections == 1)
            ){
                printf("No one to hear, clearing out message\n");
                free(send_state -> s_state -> message);
                send_state -> s_state -> message = NULL;
            }
            printf(
                "Client name %s, message %s, has sent %d, orign %d, current cons %zu\n", 
                send_state -> s_state -> client_names[client_responsible_id],
                send_state -> s_state -> message,
                send_state -> s_state -> has_sent[client_responsible_id],
                send_state -> s_state -> originator,
                send_state -> s_state -> current_connections
            );
        }
        send_state -> s_state ->messages_sent ++;
        send_state -> s_state -> has_sent[client_responsible_id] = true;
        pthread_mutex_unlock(&send_state->s_state->msg_lock);


        int client_socket_fd = send_state -> s_state -> client_socket_fds[client_responsible_id];
        char* client_name = send_state -> s_state -> client_names[send_state -> s_state -> originator];
        if(client_name == NULL){
            client_name = "Server";
        }

        char* message = send_state -> s_state -> message;
        //Format like [name] says: [message]

        size_t message_len = strlen(client_name) + strlen(" says: ") + strlen(message) + 1;

        char* send_message = malloc(4 + message_len);

        *((int*)send_message) = htons(message_len);
        sprintf(send_message+4, "%s says: %s", client_name, message);

        printf("Sending message %s from %s to client number %d\n", send_message + 4, client_name, client_responsible_id);
        write(client_socket_fd, send_message, 4 + message_len);
        printf("Successfully sent message #%d\n", send_state -> s_state -> messages_sent);

        pthread_mutex_lock(&send_state -> s_state -> msg_lock);
        printf("Determining if message to %d is last message\n", client_responsible_id);
        if(
            send_state -> s_state -> messages_sent >= send_state -> s_state -> current_connections - 1 
        ){
            printf("Message to %d is the last one, deleting data\n", client_responsible_id);
            free(send_state -> s_state -> message);
            send_state -> s_state -> message = NULL;
            send_state -> s_state -> originator = -1;
            send_state -> s_state -> messages_sent = 0;
            bzero(send_state -> s_state -> has_sent, sizeof(bool)*MAX_CONNECTIONS);
            pthread_cond_broadcast(&send_state -> s_state -> all_sent);
        }
        pthread_mutex_unlock(&send_state -> s_state -> msg_lock);

        free(send_message);
    }
    return NULL;
}

int main(int argc, char** argv){
    int socketfd = socket(AF_INET, SOCK_STREAM, 0);

    struct in_addr local_addr;
    int c;
    char* local_ip = "127.0.0.1";
    int local_port = 65535;
    while((c = getopt(argc, argv, "h:p:")) != -1){
        switch(c){
            case 'h':
                if(optarg != NULL){
                    local_ip = optarg;
                }
                break;
            case 'p':
                if(optarg != NULL){
                    local_port = atoi(optarg);
                }
                break;
        }
    }

    printf("IP %s port %d\n", local_ip, local_port);

    

    int ip_cast = inet_aton(local_ip, &local_addr);
    if(ip_cast == 0){
        perror("ip failed to translate");
        return 1;
    }

    struct sockaddr_in sock_bind;

    sock_bind.sin_family = AF_INET;
    sock_bind.sin_port = local_port;
    sock_bind.sin_addr = local_addr;

    int bindstatus = bind(socketfd, (const struct sockaddr*)&sock_bind, sizeof(struct sockaddr_in));
    if(bindstatus == -1){
        perror("bind failed");
    }

    int listenstatus = listen(socketfd, MAX_CONNECTIONS);
    if(listenstatus == -1){
        perror("socket listed failed");
    }

    server_state state;
    state.server_socket_fd = socketfd;
    
    int connection_socket_fds[MAX_CONNECTIONS];
    bzero(connection_socket_fds, MAX_CONNECTIONS*sizeof(int));

    state.client_socket_fds = connection_socket_fds;
    state.current_connections = 0;

    char* client_names[MAX_CONNECTIONS];
    bzero(client_names, MAX_CONNECTIONS*sizeof(char*));

    state.client_names = client_names;

    state.message = NULL;
    state.originator = -1;

    state.messages_sent = 0;

    pthread_mutex_t lock;
    pthread_mutex_init(&lock, NULL);

    pthread_cond_t cond;
    pthread_cond_init(&cond, NULL);

    pthread_cond_t all_sent;
    pthread_cond_init(&all_sent, NULL);

    state.msg_lock = lock;
    state.msg_cond = cond;
    state.all_sent = all_sent;

    bool* block = malloc(sizeof(bool)*MAX_CONNECTIONS);
    bzero(block, sizeof(bool)*MAX_CONNECTIONS);

    state.has_sent = block;
    s = &state;

    pthread_t threads[LISTEN_THREADS];
    for(int i = 0; i < LISTEN_THREADS; i++){
        pthread_t thread;
        pthread_create(&thread, NULL, &handle_connection, (void*)&state);
        threads[i] = thread;
    }

    sender_state sender_states[MAX_CONNECTIONS];
    for(int i = 0; i < MAX_CONNECTIONS; i++){
        sender_state s;
        s.s_state = &state;
        s.connection_handled = i;
        sender_states[i] = s;
    }
    pthread_t send_threads[MAX_CONNECTIONS];
    for(int i = 0; i < MAX_CONNECTIONS; i++){
        pthread_t thread;
        
        pthread_create(&thread, NULL, &send_message, (void*)&sender_states[i]);
        send_threads[i] = thread;
    }

    for(int i = 0; i < LISTEN_THREADS; i++){
        pthread_join(threads[i], NULL);
    }

    for(int i = 0; i < MAX_CONNECTIONS; i++){
        pthread_join(send_threads[i], NULL);
    }

    return 0;
}