#include<stdlib.h>
#include<stdio.h>
#include<unistd.h>
#include<sys/stat.h>
#include <sys/wait.h>
#include<sys/ipc.h>
#include<sys/sem.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<string.h>
#include<ctype.h> //toupper()

#define BUFFSIZE 2048
#define SEM_NUM 2
#define STOP_MSG "STOPLPROCESSES"

//ACTIVE_L gestisce il numero di processi L-i attivi
//PSEM gestisce l'attivazione di P (di default ad 1)
enum {ACTIVE_L, PSEM};

typedef struct{
    long type;
    char ch;
    int occurrences;
    char done;
}s_message;

/**
 *segnala l'errore avvenuto ed esce dal programma.
*/
void err(char *err){
    perror(err);
    exit(1);
}

int WAIT(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,-1,0}};
    return semop(sem_des, operazioni, 1);
}

int SIGNAL(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,+1,0}};
    return semop(sem_des, operazioni, 1);
}

void l_process(char i, int shmid, int msgqid, int semid);

void s_process(int msgqid, int semid);

int main(int argc, char *argv[]){
    
    if( argc != 2){
        fprintf(stderr, "sintassi errata.\n");
        exit(1);
    }

    //creazione segmento di memoria condiviso per processo P e processi L-i
    int shmid;
    if(  (shmid = shmget(IPC_PRIVATE, sizeof(char)*BUFFSIZE, IPC_CREAT | 0660 ) ) < 0 ) err("shmget P");

    //creazione coda di messaggi per processi L-i e processo S
    int msgqid;
    if( (msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0660) ) < 0 ) err("msgget");

    //creazione ed inizializzazione dei due semafori necessari per la sincronizzazione dei processi;
    int semid;
    if ( (semid = semget(IPC_PRIVATE, SEM_NUM, IPC_CREAT | 0660 )) < 0 ) err("semget");
    if( semctl(semid, ACTIVE_L, SETVAL, 0) < 0 ) err("semctl");
    if( semctl(semid, PSEM, SETVAL, 1) < 0 ) err("semctl");

    //start dei processi L-i
    for(char c = 'a'; c<='z'; c++)
        if( !fork() )
            l_process(c, shmid, msgqid, semid);

    //creazione del processo S
    if( !fork() ) s_process(msgqid, semid);

    //PROCESSO P 

    //leggere l'intero file riga per riga e mandare ogni riga ad il processo L-i
    FILE *in;
    if(  ( in = fopen(argv[1], "r")) == NULL) err("fopen");
    char buff[BUFFSIZE];

    //attaccare l'area di memoria condivisa a questo processo
    char *shmem;
    if( (shmem = (char*) shmat(shmid, NULL, SHM_W) ) == NULL  ) err("shmat P");

    int line = 0; //numero di linee lette
    while( fgets(buff, BUFFSIZE, in) ){
        if( buff[ strlen(buff) - 1 ] == '\n' ) //strip dello \n
            buff[ strlen(buff) - 1 ] = '\0';
        //INGRESSO NELL'AREA CRITICA
        WAIT(semid, PSEM);
        printf("\033[0;31m"); //rosso
        printf("[P] riga n.%i: ", ++line);
        printf("\033[0m"); //colore di default (bianco)
        printf("%s\n", buff);
        strncpy(shmem, buff, BUFFSIZE);

        //risveglio dei processi L-i
        for(int i=0; i<26; i++) SIGNAL(semid, ACTIVE_L);

        //il prossimo ciclo avverrà quando il processo S risveglierà P
    }

    //uscito dal WHILE, il processo P avrà il semaforo PSEM ad 1
    //tutti i 26 processi L-i sono in wait sul semaforo ACTIVE_L, che sarà 0
    //S sta aspettando i prossimi messaggi dai processi L-i (in wait nella chiamata msgrcv)
    //creiamo un messaggio di STOP per informare tutti i processi ACTIVE_L che abbiamo terminato la lettura
    char *stopmsg = STOP_MSG;
    WAIT(semid, PSEM);
    strncpy(shmem, stopmsg, strlen(stopmsg));
    for(int i=0; i<26; i++) SIGNAL(semid, ACTIVE_L);
    SIGNAL(semid, PSEM); 

    //wait di tutti i processi prima di eliminare le strutture IPC
    for(int i = 0; i<26+1; i++) wait(NULL);

    fclose(in);
    //distruzione area di memoria condivisa
    if( shmdt(shmem) < 0 ) err("shmdt P");
    if(  shmctl(shmid, IPC_RMID, NULL) < 0 ) err("shmctl");
    //distruzione coda di messaggi
    if(  msgctl(msgqid, IPC_RMID, NULL) < 0 ) err("msgctl");
    //distruzione dei semafori
    if( semctl(semid, 0, IPC_RMID) < 0  ) err("semctl");

    return 0;
}

void l_process(char i, int shmid, int msgqid, int semid){

    //attach dell'area di memoria condivisa
    char* shmem;
    if( (shmem = (char*) shmat(shmid, NULL, SHM_RDONLY) ) == NULL  ) err("shmat P");

    //messaggio da mandare ad S
    s_message mess;
    mess.type = 1;
    mess.ch = i;
    mess.done = 0;

    char *stopmsg = STOP_MSG;

    while(1){
        mess.occurrences = 0;

        //ingresso nell'area di memoria condivisa
        WAIT(semid, ACTIVE_L);
        if( strncmp(shmem, stopmsg, strlen(stopmsg)) == 0 ) break; //interrompe il ciclo
        for(int c = 0; c<strlen(shmem); c++){
            if( toupper((int)i) == toupper((int)shmem[c]) )
                mess.occurrences++;
        }
        if( msgsnd(msgqid, &mess, sizeof(s_message) - sizeof(long), 0 ) < 0 )err("msgsnd L");
        //TODO ??
        //usato per non permettere ad un processo di consumare due sengali del semaforo ACTIVE_L
        //durante l'esecuzione di questa istruzione PSEM sarà già 0, dato che i processi L
        //partono dopo la WAIT effettuata da P sul semaforo binario PSEM, che sarà 0
        //WAIT(semid, PSEM);
    }

    //messaggio di stop per il processo L (ne basta uno dei 26 processi)
    mess.done = 1;
    if( msgsnd(msgqid, &mess, sizeof(s_message) - sizeof(long), 0 ) < 0 )err("msgsnd L");

    if( shmdt(shmem) < 0 ) err("shmdt L");
    exit(0);
}

void s_process(int msgqid, int semid){
    unsigned int total_occur[26]; //num di occorrenze totali per ogni carattere
    unsigned int current_occur[26]; //num di occorrenze per linea per ogni carattere
    for(int i = 0; i<26; i++){
        total_occur[i] = 0;
        current_occur[i] = 0;
    }
    int messages_recieved; //numero di messaggi ricevuti (usato per capire quando stoppare il conteggio per riga)
    int line = 0;
    s_message mess;

    while(1){

        if( msgrcv(msgqid, &mess, sizeof(s_message) - sizeof(long), 0, 0) < 0 ) err("msgrcv S");
        if(mess.done) break;
        messages_recieved++;
        total_occur[mess.ch - 'a'] += mess.occurrences;
        current_occur[mess.ch - 'a'] = mess.occurrences;

        //se abbiamo ricevuto tutti i messaggi, print delle stat della linea e risvegliamo P
        if(messages_recieved == 26){
            printf("\033[0;31m"); //rosso
            printf("[S] riga n.%i: ", ++line);
            printf("\033[0m"); //colore di default (bianco)
            for(int i=0; i<26; i++){
                printf( "%c=%i ", (char) i+'a', current_occur[i] );
            }
            printf("%c", '\n');
            for(int i=0; i<26; i++) current_occur[i] = 0;
            messages_recieved = 0;
            SIGNAL(semid, PSEM);
        }

    }

    //print delle stat finali
    printf("\033[0;31m"); //rosso
    printf("[S] intero file: ");
    printf("\033[0m"); //colore di default (bianco)
    for(int i=0; i<26; i++){
        printf( "%c=%i ", (char) i+'a', total_occur[i] );
    }
    printf("%c", '\n');
    exit(0);
}