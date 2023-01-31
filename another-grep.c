#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/mman.h>
#include<sys/ipc.h>
#include<sys/msg.h>
#include<string.h>
#include<wait.h>
#include<fcntl.h>

#define BUFFSIZE 1024

typedef struct{
    long type;
    char message[BUFFSIZE];
    char word[BUFFSIZE];
    char done;
}queue_message;

void err(char *error){
    perror(error);
    exit(1);
}
//pipe unidirezionale
void readProcess(char *filename, int* pipedes){

    close(pipedes[0]); //chiusura del canale di lettura del pipe

    //MAP DEL FILE IN MEMORIA
    int in;
    if( (in = open(filename, O_RDWR)) < 0 ) err("open");
    //dimensione del file
    struct stat statbuff;
    stat(filename, &statbuff); 
    char* file;
    if( (file = (char*) mmap(NULL, statbuff.st_size+sizeof(char), PROT_READ | PROT_WRITE, MAP_PRIVATE ,in, 0) ) == NULL ) 
        err("mmap");

    file[strlen(file)] = '\0';
    
    if( write(pipedes[1], file, statbuff.st_size+sizeof(char)) <0  ) err("write");

    munmap(file, statbuff.st_size+sizeof(char));
    close(in);
    close(pipedes[1]);
    exit(0);
}

void writeProcess(int msgq){
    queue_message msg;
    while(1){
        if( msgrcv(msgq, &msg, sizeof(queue_message) - sizeof(long), 0, 0) < 0 ) err("msgrcv");
        if(msg.done) break;
        //stampa colorata
        char *entry = strtok(msg.message, " ");
        while(entry != NULL){
            printf("\033[0m"); //colore di default (bianco)
            if( strncasecmp(msg.word, entry, BUFFSIZE) == 0 ) printf("\033[0;31m"); //rosso
            printf("%s ", entry);
            entry = strtok(NULL, " ");
        }
        printf("\n");
    }
    exit(0);
}

int in(char *word, char* string){
    char tmp[BUFFSIZE]; //per non modificare la riga contenuta in line
    strncpy(tmp, string, BUFFSIZE);
    char *entry = strtok(tmp, " ");
    while(entry != NULL){
        if( strncasecmp(word, entry, BUFFSIZE) == 0 ) return 1;
        entry = strtok(NULL, " ");
    }
    return 0;
}

int main(int argc, char *argv[]){

    if(argc != 3 ){
        puts("sintassi errata.");
        exit(1);
    }
    
    //CREAZIONE PIPE PER R -> P
    int *pipedes = (int*) malloc(sizeof(int)*2); //filedes[0] lettura, filedes[1] scrittura
    if( pipe(pipedes) < 0 ) err("pipe");

    //CREAZIONE CODA PER P -> W
    int msgq;
    if ( ( msgq = msgget(IPC_PRIVATE,  IPC_CREAT | IPC_EXCL | 0660) ) < 0 ) err("msgget");

    //R (readProcess)
    if( !fork() ) readProcess(argv[2], pipedes);

    //W (writeProcess)
    int Wpid = fork();
    if( !Wpid ) writeProcess(msgq);

    //PROCESSO P
    queue_message msg;
    msg.type = 1;
    msg.done = 0;
    strncpy(msg.word, argv[1], BUFFSIZE); //usato per la sintassi colorata
    char singlechar;
    char line[BUFFSIZE];
    int lineindx = 0;
    for(int i=0; i<BUFFSIZE; i++) line[i] = '\0'; //reset di line
    int control; //usato per l'ultima riga del file: se è 0, EOF raggiunto (necessario per leggere anche l'ultima riga del file)
    close(pipedes[1]); //chiusura del canale di scrittura per la corretta rilevazione dell'EOF
    while( 1 ){
        if( (control = read(pipedes[0], &singlechar, 1)) < 0 ) err("read");
        if(singlechar == '\0'){
            singlechar = '\n'; //EOF
            control = 0;
        }
        if(singlechar != '\n'){
            line[lineindx++] = singlechar;
        }else{
            if( !in(argv[1], line) ){
                if(!control) break;
                for(int i=0; i<BUFFSIZE; i++) line[i] = '\0';
                lineindx = 0;
                continue; //non è presente la parola all'interno del buffer
            } 
            //è presente la parola, mandiamo un messaggio al processo W
            strncpy(msg.message, line, BUFFSIZE);
            if( msgsnd(msgq, &msg, sizeof(queue_message) - sizeof(long), 0) < 0 ) err("msgsnd");
            if(!control) break;
            for(int i=0; i<BUFFSIZE; i++) line[i] = '\0';
            lineindx = 0;
        }
    }

    //comunichiamo al processo W di interrompersi
    msg.done = 1;
    if( msgsnd(msgq, &msg, sizeof(queue_message) - sizeof(long), 0) < 0 ) err("msgsnd");

    //wait per il processo di scrittura prima di chiudere la coda di messaggi
    waitpid(Wpid, NULL, 0);

    close(pipedes[0]);
    free(pipedes);
    msgctl(msgq, IPC_RMID, NULL);

    return 0;
}