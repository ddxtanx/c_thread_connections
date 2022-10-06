#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
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
#include <termios.h>

bool debug = false;
size_t MAX_MESSAGE_SIZE = 256;
size_t MAX_MESSAGE_QUEUE_SIZE = 100;
pthread_t* input_thread_ptr = NULL;
pthread_t* network_recv_thread_ptr = NULL;
pthread_t* network_send_thread_ptr = NULL;
pthread_t* print_msg_thread_ptr = NULL;
struct termios orig_state;


typedef struct _client_state{
    int client_socket;
    int client_id;
    char* client_name;

    char** messages_queue;
    size_t num_in_queue;

    bool is_writing;
    bool has_prompt;
    char* message;
    size_t message_len;

    bool exited;

    pthread_mutex_t write_lock;
    pthread_mutex_t recv_lock;

    pthread_cond_t can_print_cond;
    pthread_cond_t waiting_msg_cond;
    pthread_cond_t can_send_cond;
    pthread_cond_t full_queue_cond;
} client_state;

client_state* state = NULL;

void* server_message_handler(void* client_state_ptr){
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    client_state* state = client_state_ptr;
    int socketfd = state -> client_socket;

    int new_msg_len;
    while(1){
        if(debug) printf("Recieving message from server\n");
        size_t read_bytes = recv(socketfd, &new_msg_len, 4, MSG_WAITALL);
        if(read_bytes != 4){
            printf("\n server closed, only read %zu bytes\n", read_bytes);
            pthread_cancel(*input_thread_ptr);
            return NULL;
        }
        if(debug) printf("Recieved message \n");

        if(new_msg_len == 0){
            break;
        }

        new_msg_len = ntohs(new_msg_len);
        if(debug) printf("Of length %d\n", new_msg_len);

        char* server_msg = malloc(new_msg_len);
        recv(socketfd, server_msg, new_msg_len, MSG_WAITALL);

        pthread_mutex_lock(&state -> recv_lock);
        while(state -> num_in_queue == MAX_MESSAGE_QUEUE_SIZE){
            pthread_cond_wait(&state -> full_queue_cond, &state -> recv_lock);
        }
        size_t free_index = state -> num_in_queue;
        //printf("Recieved message %s\n", server_msg);
        state -> messages_queue[free_index] = server_msg;
        state -> num_in_queue ++;
        pthread_cond_broadcast(&state -> can_print_cond);
        pthread_mutex_unlock(&state -> recv_lock);
    }
    return NULL;
}

void* write_message_handler(void* client_state_ptr){
    client_state* state = client_state_ptr;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while(1){
        pthread_mutex_lock(&state -> write_lock);
        while(state -> message != NULL){
            pthread_cond_wait(&state -> waiting_msg_cond, &state -> write_lock);
        }
        char* msg = malloc(MAX_MESSAGE_SIZE);
        msg[0] = '\0';
        state -> message = msg;
        state -> message_len = 0;
        if(!(state -> has_prompt)){
            printf("Enter a message (max size %zu, quit to end): ", MAX_MESSAGE_SIZE);
            state -> has_prompt = true;
        }
        
        pthread_mutex_unlock(&state -> write_lock);
        size_t real_size = 0;

    
        for(int i = 0; i < MAX_MESSAGE_SIZE-1; i++){
            int c = fgetc(stdin);
            state -> is_writing = true;
            pthread_mutex_lock(&state -> write_lock);
            if(c == EOF || c == '\n'){
                printf("\r");
                pthread_mutex_unlock(&state -> write_lock);
                break;
            } else if((c == 127 || c == 8) && i != 0){ // backspace or delete
                msg[i-1] = '\0';
                printf("\rEnter a message (max size %zu, quit to end): %s ", MAX_MESSAGE_SIZE, msg);
                printf("\rEnter a message (max size %zu, quit to end): %s", MAX_MESSAGE_SIZE, msg);
                real_size--;
                i -= 2;
            } else if ((isalnum(c) || ispunct(c)) && !iscntrl(c)){
                msg[i] = c;
                msg[i+1] = '\0';
                real_size++;
                printf("%c", c);
            } else{ // Non visible char, dont print
                i--;
            }
            pthread_mutex_unlock(&state -> write_lock);
        }
        state -> is_writing = false;
        msg[real_size] = '\0';

        if(strcmp(msg, "quit") == 0){
            free(msg);
            break;
        }
        char* templ_str = ": ";
        size_t total_message_len = snprintf(NULL, 0, "\rEnter a message (max size %zu, quit to end): %s", MAX_MESSAGE_SIZE, msg);
        size_t real_message_len = strlen(templ_str) + strlen(state -> client_name) + real_size;
        size_t diff = 0;
        if(real_message_len < total_message_len){
            diff = total_message_len - real_message_len;
        }
        
        printf(
            "%s: %s%*s\n", 
            state -> client_name, 
            msg,
            diff,
            ""
        );

        state -> has_prompt = false;

        pthread_mutex_lock(&state -> write_lock);
        
        state -> message_len = real_size + 1;
        pthread_cond_broadcast(&state -> can_send_cond);
        pthread_mutex_unlock(&state -> write_lock);
    }
    return NULL;
}

void* send_message_handler(void* client_state_ptr){ //formatted like [len]m[client id][message]
    client_state* state = client_state_ptr;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while(1){
        pthread_mutex_lock(&state -> write_lock);
        while(state -> message == NULL || state -> message_len == 0){
            pthread_cond_wait(&state -> can_send_cond, &state -> write_lock);
        }
        char* msg = state -> message;
        size_t msg_size = state -> message_len;
        pthread_mutex_unlock(&state -> write_lock);

        state -> message = NULL;
        state -> message_len = 0;

        int client_id = state -> client_id;

        size_t total_len = 1 + sizeof(int) + msg_size;

        size_t send_msg_len = sizeof(int) + total_len;
        char* send_msg = malloc(send_msg_len);
        *((int*)send_msg) = htons(total_len);
        send_msg[4] = 'm';
        *((int*)(send_msg + 5)) = htons(client_id);
        strcpy(send_msg + 2*sizeof(int) + 1, msg);

        send(state -> client_socket, send_msg, send_msg_len, 0);

        free(send_msg);
        free(msg);
        pthread_cond_broadcast(&state ->waiting_msg_cond);
    }
}

void* print_message_handler(void* client_state_ptr){
    client_state* state = client_state_ptr;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while(1){
        pthread_mutex_lock(&state -> write_lock);
        while(state -> num_in_queue == 0){
            pthread_cond_wait(&state -> can_print_cond, &state -> write_lock);
        }
        for(int i = 0; i < state -> num_in_queue; i++){
            char* message = state -> messages_queue[i]; 
            size_t templ_len;
            if(state -> message != NULL){
                templ_len = snprintf(NULL, 0, "Enter a message (max size %zu, quit to end): %s", MAX_MESSAGE_SIZE, state -> message);
            } else{
                templ_len = snprintf(NULL, 0, "Enter a message (max size %zu, quit to end): ", MAX_MESSAGE_SIZE);
            }
            size_t mes_len = strlen(message);
            size_t diff = 0;
            if(templ_len > mes_len){
                diff = templ_len - mes_len;
            }

            printf("\r%s%*s\n", message, diff, "");
            fflush(stdout);
            if(state -> message != NULL){
                printf("Enter a message (max size %zu, quit to end): %s", MAX_MESSAGE_SIZE, state -> message);
            } else{
                printf("Enter a message (max size %zu, quit to end): ", MAX_MESSAGE_SIZE);
            }
            
            state -> has_prompt = true;
            fflush(stdout);
            free(message);
            state -> messages_queue[i] = NULL;
        }
        state -> num_in_queue = 0;
        pthread_cond_broadcast(&state -> full_queue_cond);
        pthread_mutex_unlock(&state -> write_lock);
    }

}

void init_state(client_state* s, int socketfd, char* name){
    s->client_socket = socketfd;
    s -> client_name = name;

    pthread_mutex_t lock;
    pthread_mutex_t lock2;
    pthread_cond_t cond_1;
    pthread_cond_t cond_2;
    pthread_cond_t cond_3;
    pthread_cond_t cond_4;

    pthread_cond_init(&cond_1, NULL);
    pthread_cond_init(&cond_2, NULL);
    pthread_cond_init(&cond_3, NULL);
    pthread_cond_init(&cond_4, NULL);
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&lock2, NULL);

    s->write_lock = lock;
    s -> recv_lock = lock2;
    s->can_print_cond = cond_1;
    s->full_queue_cond = cond_2;
    s->waiting_msg_cond = cond_3;
    s->can_send_cond = cond_4;

    char** message_queue = malloc(sizeof(char*) * MAX_MESSAGE_QUEUE_SIZE);
    s->messages_queue = message_queue;
    bzero(message_queue, MAX_MESSAGE_QUEUE_SIZE);

    s->num_in_queue = 0;
    s->is_writing = false;
    s -> has_prompt = false;

    s -> message = NULL;
}

void destroy_state(client_state* s){
    if(input_thread_ptr != NULL){
        pthread_cancel(*input_thread_ptr);
    } 
    if(network_recv_thread_ptr != NULL){
        pthread_cancel(*network_recv_thread_ptr);
    }

    if(network_send_thread_ptr!= NULL){
        pthread_cancel(*network_send_thread_ptr);
    }

    if(print_msg_thread_ptr != NULL){
        pthread_cancel(*print_msg_thread_ptr);
    }

    pthread_mutex_unlock(&s -> write_lock);
    pthread_mutex_destroy(&s -> write_lock);

    pthread_mutex_unlock(&s -> recv_lock);
    pthread_mutex_destroy(&s -> recv_lock);

    pthread_cond_broadcast(&s -> can_print_cond);
    pthread_cond_destroy(&s -> can_print_cond);

    pthread_cond_broadcast(&s -> can_send_cond);
    pthread_cond_destroy(&s -> can_send_cond);

    pthread_cond_broadcast(&s -> full_queue_cond);
    pthread_cond_destroy(&s -> full_queue_cond);

    pthread_cond_broadcast(&s -> waiting_msg_cond);
    pthread_cond_destroy(&s -> waiting_msg_cond);

    free(s -> messages_queue);
    free(s -> client_name);
    char close_con[4+1+4];

    *((int*)close_con) = htons(1+4);
    close_con[4] = 'c';
    *((int*)(close_con+5)) = htons(state -> client_id);

    write(s -> client_socket, (void*)close_con, 4+1+4);
    close(s -> client_socket);
    if(s -> message != NULL){
        free(s -> message);
    }
}

void sigint_handler(int signum){
    if(state != NULL){
        destroy_state(state);
    }

    if(input_thread_ptr != NULL){
        pthread_cancel(*input_thread_ptr);
    } 
    if(network_recv_thread_ptr != NULL){
        pthread_cancel(*network_recv_thread_ptr);
    }

    if(network_send_thread_ptr!= NULL){
        pthread_cancel(*network_send_thread_ptr);
    }

    if(print_msg_thread_ptr != NULL){
        pthread_cancel(*print_msg_thread_ptr);
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_state);
    exit(signum);
}

int main(int argc, char** argv){
    signal(SIGINT, sigint_handler);
    signal(SIGSEGV, sigint_handler);

    int socketfd = socket(AF_INET, SOCK_STREAM, 0);

    struct in_addr local_addr;

    char* local_ip = "127.0.0.1";
    int local_port = 65535;
    int c;
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

    struct sockaddr_in sock_connect;

    sock_connect.sin_family = AF_INET;
    sock_connect.sin_port = local_port;
    sock_connect.sin_addr = local_addr;

    int connectstatus = connect(socketfd, (const struct sockaddr*)&sock_connect, sizeof(struct sockaddr_in));

    // Getting User Name
    char* name = malloc(MAX_MESSAGE_SIZE);
    
    printf("Name (max 1000 letters): ");
    fgets(name, MAX_MESSAGE_SIZE, stdin);

    tcgetattr(STDIN_FILENO, &orig_state);

    struct termios term_state = orig_state;
    
    term_state.c_lflag &= ~(ECHO | ICANON);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_state);

    size_t name_len = strlen(name);

    if(debug) printf("Len of %s is %zu\n", name , name_len);

    if ((name_len > 0) && (name[name_len - 1] == '\n'))
        name[name_len - 1] = '\0';

    name_len--;

    if(debug) printf("Len of %s is %zu\n", name , name_len);

    name = realloc(name, name_len+1);

    client_state s;
    init_state(&s, socketfd, name);

    state = &s;

    if(debug) printf("Len of %s is %zu\n", name , name_len);

    size_t len = htons(name_len+2);
    size_t msg_len = sizeof(int) + name_len + 2;
    char* msg = malloc(msg_len);

    *((int*)msg) = len;
    msg[4] = 'n';
    strcpy(msg+5, name);
    if(debug) printf("Num bytes: %d, strlen of data is %zu\n", ntohs(*(int*)msg), strlen(msg+4));
    if(debug) printf("Message: %s\n", msg+sizeof(int));

    if(debug) printf("Writing message \n");
    write(socketfd, msg, msg_len);
    if(debug) printf("Wrote message \n");
    free(msg);

    int num_bytes;
    if(debug) printf("reading response\n");
    read(socketfd, (void*)&num_bytes, 4);

    if(debug) printf("Response len %d\n", ntohs(num_bytes));

    if(num_bytes == 0){
        if(debug) printf("Error on connection\n");
        close(socketfd);
        return 1;
    }

    num_bytes = ntohs(num_bytes);
    
    char* resp = malloc(num_bytes);

    read(socketfd, (void*)resp, num_bytes);

    size_t client_id = ntohs(*(int*)resp);

    s.client_id = client_id;

    free(resp);

    if(debug) printf("Response: %zu\n", client_id);

    char* arg_msg;
    size_t arg_len;
    if(argc == 3){
        arg_msg = argv[2];
        arg_len = strlen(arg_msg)+1;
    } else{
        arg_msg = malloc(8);
        strcpy(arg_msg, "default");
        arg_len = 8;
    }

    // if(debug) printf("Atttempting to send message %s\n", arg_msg);

    // char* arg_send = malloc(2*4 + arg_len);
    // *((int*)arg_send) = htons(4 + arg_len);
    // *((int*)(arg_send) + 1) = htons(client_id);
    // strcpy(arg_send + 8, arg_msg);

    // if(debug) printf("Sending message of len %zu\n", 2*4+arg_len);
    // write(socketfd, arg_send, 2*4 + arg_len);
    // free(arg_msg);
    // free(arg_send);
    // if(debug) printf("Sent\n");

    pthread_t input_thread;
    pthread_t network_recv_thread;
    pthread_t network_send_thread;
    pthread_t print_msg_thread;

    input_thread_ptr = &input_thread;
    network_recv_thread_ptr = &network_recv_thread;
    network_send_thread_ptr = &network_send_thread;
    print_msg_thread_ptr = &print_msg_thread;

    pthread_create(&input_thread, NULL, &write_message_handler, &s);
    pthread_create(&network_recv_thread, NULL, &server_message_handler, &s);
    pthread_create(&network_send_thread, NULL, &send_message_handler, &s);
    pthread_create(&print_msg_thread, NULL, &print_message_handler, &s);

    pthread_join(input_thread, NULL);

    destroy_state(&s);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_state);

    exit(0);
    return 0;
}