#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/ipc.h>
#include<sys/msg.h>
#include<string.h>
#include<wait.h>

#define BUFFSIZE 1024

//STRUCT PER MESSAGGI

typedef struct{
    long type;
    char word[BUFFSIZE];
    char done;
}request_message;

typedef struct{
    long type;
    char line[BUFFSIZE];
    uint line_indx;
    int id; //per identificare il mittente del messaggio
    char word[BUFFSIZE];
    char done;
}response_message;


//STRUCT E DATI PER LISTA
struct Node{

    struct Node *next;

    char line[BUFFSIZE];
    uint line_indx;
};

typedef struct Node Node;

typedef struct{
    Node *head;
    Node *tail;
}List;


void err(char *error){
    perror(error);
    exit(1);
}

//crea un nuovo nodo con 
void addNode(List *l, char *line, uint line_indx){
    
    Node *n = (Node*) malloc(sizeof(Node));

    strncpy(n->line, line, BUFFSIZE);
    if( n->line[strlen(n->line)-1] == '\n')
        n->line[strlen(n->line)-1] = '\0';

    n->line_indx = line_indx;
    n->next = NULL;

    if(l->head == NULL && l->tail == NULL){
        l->head = n;
        l->tail = n;
    }else{
        //add in coda alla lista
        if(l->head->next == NULL) l->head->next = l->tail;
        l->tail->next = n;
        l->tail = n;
    }

}

List* loadFile(char *file){

    FILE *in;
    if( (in = fopen(file, "r")) == NULL ) err("fopen");
    
    //init della lista
    List *list = (List*) malloc(sizeof(List));
    list->head = NULL;
    list->tail = NULL;
    
    char buff[BUFFSIZE];
    uint line=1;

    while( fgets(buff, BUFFSIZE, in) )
        addNode(list, buff, line++);

    if( fclose(in) < 0 ) err("fclose");

    return list;
}

//METODO DI DEBUG -- IGNORARE
void printlist(List *l){
    Node *cur = l->head;
    while ( 1 ){
        if( cur == NULL ) break;
        printf("%s, %i\n", cur->line, cur->line_indx);
        cur = cur->next; 
    }
}

void destroy_list(List *l){
    Node *cur = l->head;
    Node *tmp = NULL;
    while( 1 ){
        if(cur == NULL) break;
        tmp = cur->next;
        free(cur);
        cur = tmp;
    }
    free(l);
}

void child_process(int id, char *file, int req_msgqid, int res_msgqid){

    //carichiamo l'intero file in una lista contenente per ogni nodo la riga ed l'indice di riga
    List* l = loadFile(file);

    request_message req_msg;

    response_message res_msg;
    res_msg.type = 1;
    res_msg.done = 0;
    res_msg.id = id;
    
    while( 1 ){

        //adesso possiamo aspettare la richiesta dal padre
        if( msgrcv(req_msgqid, &req_msg, sizeof(req_msg) - sizeof(long), id, 0) < 0 ) err("msgrcv CHILD");

        if(req_msg.done)
            break;

        //impostiamo la parola corretta
        strncpy(res_msg.word, req_msg.word, BUFFSIZE);

        //adesso abbiamo la parola: cerchiamo le sue occorrenze all'interno del file
        Node *cur = l->head;
        while ( 1 ){
            if( cur == NULL) break;
            //riscontrata occorrenza: inviamo un messaggio al padre
            if( strstr(cur->line, req_msg.word) != NULL ){
                strncpy( res_msg.line, cur->line, BUFFSIZE);
                res_msg.line_indx = cur->line_indx;
                if( msgsnd(res_msgqid, &res_msg, sizeof(res_msg) - sizeof(long), 0) < 0 ) err("msgsnd CHILD");

            }
            cur = cur->next;
        }

    }

    //inviamo un messaggio al padre segnalando di aver terminato
    res_msg.done = 1;
    if( msgsnd(res_msgqid, &res_msg, sizeof(res_msg) - sizeof(long), 0) < 0 ) err("msgsnd CHILD");

    //destroy della lista
    destroy_list(l);

    exit(0);
}

int main(int argc, char *argv[]){

    if(argc<4){
        fprintf(stderr, "sintassi errata. Utilizzo corretto: %s <word-1> [word-2] [...] @ <file-1> [file-2] [...]\n", argv[0]);
        exit(1);
    }

    //creare le code di messaggi (richiesta e risposta)
    int req_msgqid;
    int res_msgqid;
    if( (req_msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0660)) < 0 ) err("msgget P");
    if( (res_msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0660)) < 0 ) err("msgget P");

    //CREAZIONE PROCESSI FIGLIO
    int words_num = 0;
    int file_num = 0;
    char at_encountered = 0; //true se abbiamo riscontrato il carattere @
    for(int i=1; i<argc; i++){

        if( !at_encountered ){
            if( argv[i][0] == '@' ){
                at_encountered = 1;
                continue;
            }
        }
        
        if(!at_encountered) words_num++;
        else file_num++;

    }

    //creiamo i processi passando ad ognuno di essi il file corretto
    for(int id=1; id<=file_num; id++)
        if(fork() == 0)
            child_process(id, argv[words_num+1+id], req_msgqid, res_msgqid);
    
    //PROCESSO P

    //inviare ad ogni processo figlio le parole da cercare
    //num processi figlio = num file
    request_message req_msg;
    req_msg.done = 0;
    for(int id=1; id<=file_num; id++){
        req_msg.type = id;
        for(int i=0; i<words_num; i++){
            strncpy(req_msg.word, argv[i+1], BUFFSIZE);
            if( msgsnd(req_msgqid, &req_msg, sizeof(req_msg) - sizeof(long), 0) < 0 ) err("msgsnd P");
        }
    }

    //inviamo i messaggi di stop ai processi child
    //dopo che avranno processato tutte le parole, vedranno i messaggi di stop
    req_msg.done = 1;
    for(int id=1; id<=file_num; id++){
        req_msg.type = id;
        if( msgsnd(req_msgqid, &req_msg, sizeof(req_msg) - sizeof(long), 0) < 0 ) err("msgsnd P");
    }

    //mettiamoci in attesa delle risposte di tutti i processi figli
    response_message res_msg;

    int ended_children = 0; //figli che hanno terminato l'elaborazione

    uint occurrences[file_num];
    for(int i=0; i<file_num; i++) occurrences[i] = 0;
    
    while( 1 ){
        //attesa delle risposte
        if( msgrcv(res_msgqid, &res_msg, sizeof(res_msg) - sizeof(long), 0, 0) < 0 ) err("msgrcv CHILD");

        //controllo per verificare se tutti i processi child hanno terminato l'elaborazione
        if(res_msg.done){
            ended_children++;
            if(ended_children == file_num) break;
            continue;
        }
        
        //print del messaggio: parola@nome-file:numero-di-linea:contenuto-intera-riga
        printf("\033[0;31m"); //rosso
        printf("%s", res_msg.word);
        printf("\033[0m"); //bianco
        printf("@%s:%u:%s\n", 
            argv[words_num+1+res_msg.id], res_msg.line_indx, res_msg.line);
        
        //incrementiamo le occorrenze per quel file
        occurrences[res_msg.id-1]++;

    }

    //print delle occorrenze totali per ogni file
    for(int i=0; i<file_num; i++){
        printf("%s:%u\n", argv[words_num+2+i], occurrences[i]);
    }

    //attendiamo i processi figli prima di rimuovere le strutture ipc
    for(int i=0; i<file_num; i++)
        wait(NULL);

    if(msgctl(req_msgqid, IPC_RMID, NULL) < 0) err("msgctl P");
    if(msgctl(res_msgqid, IPC_RMID, NULL) < 0) err("msgctl P");

    return 0;

}