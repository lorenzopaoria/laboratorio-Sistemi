#include<stdio.h>
#include<stdlib.h>
#include <unistd.h>
#include<sys/types.h>
#include<sys/ipc.h>
#include<sys/msg.h>
#include<string.h>
#include<wait.h>
//code di messaggi
#define MAX_WORD_LEN 50

struct node{
    char key[MAX_WORD_LEN];
    struct node *next;
};

typedef struct node node;

typedef struct{//non faccio una struct list poiche ogni volta che la devo inizializzare mi devo richiamare struct list invece di list soltanto
    node *head;
}list;

//messaggio da inviare al processo comparer
typedef struct{
    long type;
    char first[MAX_WORD_LEN];
    char second[MAX_WORD_LEN];
    int done; //usato una volta creata la lista dal processo sorter
}comp_msg;

//messaggio da inviare al processo sorter
typedef struct{
    long type;
    int more; // >0 se comp_msg.first > comp_msg.second
}sort_msg;

typedef struct{
    long type;
    char entry[MAX_WORD_LEN];
    int done;
}parent_msg;

void signalErr(char *error){
    perror(error);
    exit(1);
}

//aggiunge un nuovo nodo in coda e lo inserisce nella posizione corretta usando comparer
//ritorna una istanza della lista modificata
list* appendOrder(list* l, char* value, int msgq){
    node *n = (node*) malloc(sizeof(node));
    strncpy(n->key, value, MAX_WORD_LEN);
    n->next = NULL;
    if( !l->head){
        l->head = n;
    }else{
        //aggiunta di un nuovo nodo in testa
        n->next = l->head;
        l->head = n;
        //ORDINAMENTO DELLA CODA TRAMITE IL PROCESSO COMPARER
        comp_msg cmsg; //mess che manda
        sort_msg smsg; //mess che riceve

        node* first = l->head;
        node* second = l->head->next;
        while(1){
            if(second == NULL) break;
            cmsg.type = 1;
            strncpy(cmsg.first, first->key, MAX_WORD_LEN);
            strncpy(cmsg.second, second->key, MAX_WORD_LEN);  
            cmsg.done = 0;      
            //manda il messaggio
            if( msgsnd(msgq, &cmsg, sizeof(comp_msg) - sizeof(long), 0) < 0) signalErr("msgsnd appendOrder");
            //aspetta la risposta
            if( msgrcv(msgq, &smsg, sizeof(sort_msg) - sizeof(long), 0, 0) < 0) signalErr("msgrcv appendOrder");
            
            //swap dei valori all'interno dei nodi
            if(smsg.more > 0){
                char tmp[MAX_WORD_LEN];
                strncpy(tmp, first->key, MAX_WORD_LEN);
                strncpy(first->key, second->key, MAX_WORD_LEN);
                strncpy(second->key, tmp, MAX_WORD_LEN);
                first = second;
                second = second->next;
                
            }else
                break; //siamo della posizione corretta
        }

    }

    return l;
}

//crea la lista di parole ordinandola ad ogni inserimento
//notifica il processo comparer di interrompersi
//ritorna la lista al processo sorter
list *createList(char *filename, int msgq){

    FILE *in;
    char buff[MAX_WORD_LEN];
    list *l = (list*) malloc(sizeof(list));
    l->head = NULL;

    if( (in = fopen(filename, "r")) == NULL ) signalErr("fopen");

    while( fgets(buff, MAX_WORD_LEN, in) ){
        if(buff[strlen(buff)-1] == '\n')
                buff[strlen(buff)-1] = '\0';
        l = appendOrder(l, buff, msgq); //inserisce il nuovo elemento nella lista e la ordina
    }

    //mesaggio di stop al processo COMPARER
    comp_msg stopmsg;
    stopmsg.type=1;
    stopmsg.done = 1;
    if( msgsnd(msgq, &stopmsg, sizeof(comp_msg) - sizeof(long), 0) < 0) signalErr("msgsnd createList stop");
    fclose(in);
    return l;
}

void destroyList(list *l){
    node* cur = l->head;
    node* next = l->head;
    while(next != NULL){
        next = cur->next;
        free(cur);
        cur = next;
    }
    free(l);
}

void printList(list *l){
    node* cur = l->head;
    while(cur != NULL){
        puts(cur->key);
        cur = cur->next;
    }
}

//sorta gli elementi all'interno del file
void sorter(char *filename, int msgq){

    list *l = createList(filename, msgq); //LISTA GIA' SORTATA
    //printList(l);
    //manda messaggi al parent
    parent_msg mess;
    mess.type = 1;
    node *cur = l->head;
    mess.done = 0;
    while(cur != NULL){
        strncpy(mess.entry, cur->key, MAX_WORD_LEN);
        if ( msgsnd(msgq, &mess, sizeof(parent_msg) - sizeof(long), 0) < 0 ) signalErr("msgsnd sorter");
        cur = cur->next;
    }

    mess.done = 1;
    if ( msgsnd(msgq, &mess, sizeof(parent_msg) - sizeof(long), 0) < 0 ) signalErr("msgsnd sorter stop");

    destroyList(l);
    exit(0);
}

//inserisce all'interno della coda un intero positivo se first > second
void comparer(int msgq){
    comp_msg cmsg; //mess che riceve
    sort_msg smsg; //mess che manda
    smsg.type = 1;

    while(1){
        if( msgrcv(msgq, &cmsg, sizeof(comp_msg) - sizeof(long), 0, 0) < 0) signalErr("msgrcv comparer");
        if(cmsg.done) break;
        smsg.more = strncasecmp(cmsg.first, cmsg.second, MAX_WORD_LEN);
        if( msgsnd(msgq, &smsg, sizeof(sort_msg) - sizeof(long), 0) < 0 ) signalErr("msgsnd");
    }

    exit(0);
}

int main(int argc, char *argv[]){

    if(argc != 2){
        fprintf(stderr, "sintassi errata.\n");
        return(1);
    }

    int msgq;
    
    if(  (msgq = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0660)) < 0 ) signalErr("msgget");


    //SORTER
    if(!fork()) sorter(argv[1], msgq);

    //COMPARER
    int comparerID = fork();
    if(!comparerID) comparer(msgq);

    if(comparerID){ waitpid(comparerID, NULL, 0); }

    parent_msg mess;
    
    //PARENT PROCESS (da runnare solo quando il comparer ha finito)
    while(1){
        if( msgrcv(msgq, &mess, sizeof(parent_msg) - sizeof(long), 0, 0) < 0) signalErr("msgrcv parent");
        if(mess.done) break;
        puts(mess.entry);
    }
    
    msgctl(msgq, IPC_RMID, NULL);
    return 0;
}