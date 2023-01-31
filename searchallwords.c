#define _GNU_SOURCE

#include <stdio.h> //printf, perror
#include <stdlib.h> //exit
#include <unistd.h> //pipe
#include <sys/types.h> 
#include <sys/ipc.h> //ipc
#include <sys/shm.h> //shared memory
#include <sys/sem.h> //semaphores
#include <sys/wait.h> //wait
#include <math.h> //pow
#include<string.h> 

#define N_SEMS 2 //QUESTO PROGRAMMA USA 2 SEMAFORI
#define R_SEM 0
#define ACTIVE_W 1

#define BUFFSIZE 2048

#define STOPMSG "STOPWPROCESS"

typedef struct{
    char line[BUFFSIZE];
    long int map;
    char done; //1 se R ha finito
}shared_mem;

void err(char *msg){
    perror(msg);
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


void r_process(char *filename, int semid, int shmid, int word_num, int pipefd[2]);

void w_process(char *word, int index, int semid, int shmid);

//le operazioni di read su o_process sono atomiche (niente IPC)
void o_process(int pipefd[2]);

//PROCESSO P
int main(int argc, char *argv[]){

    if( argc < 3){
        fprintf(stderr, "utilizzo non valido.\n");
        exit(1);
    }

    //CREAZIONE DEL PIPE
    int pipefd[2]; //0: pipe in lettura, 1: pipe in scrittura
    if ( pipe(pipefd) < 0 ) err("pipe P");

    //CREAZIONE SEMAFORI
    int semid;
    if( ( semid = semget(IPC_PRIVATE, N_SEMS, IPC_CREAT | 0660) ) < 0 ) err("semget P");
    if( semctl(semid, R_SEM, SETVAL, 0) < 0 ) err("semctl R");
    if( semctl(semid, ACTIVE_W, SETVAL, 0) < 0 ) err("semctl R");

    //CREAZIONE AREA DI MEMORIA CONDIVISA
    int shmid;
    if ( (shmid = shmget(IPC_PRIVATE, sizeof(shared_mem), IPC_CREAT | 0660 ) ) < 0 ) err("shmget R");

    //CREAZIONE PROCESSI
    int word_num = argc - 2;

    if ( fork() == 0 ) r_process(argv[1], semid, shmid, word_num, pipefd);

    for(int i=0; i<word_num; i++)
        if( fork() == 0)
            w_process(argv[i+2], i, semid, shmid);

    if( fork() == 0 ) o_process(pipefd);

    //wait su tutti i figli
    for(int i=0; i<word_num+1+1; i++)
        wait(NULL);

    //DISTRUZIONE STRUTTURE IPC
    close(pipefd[0]);
    close(pipefd[1]);
    semctl(semid, 0, IPC_RMID, NULL);
    shmctl(shmid, IPC_RMID, NULL);

    return 0;

}

void r_process(char *filename, int semid, int shmid, int word_num, int pipefd[2]){
    
    //area di memoria del processo
    
    shared_mem *mem;
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem *) -1 ) err("shmat");
    close(pipefd[0]); //chiusura dello stream di lettura della pipe (dobbiamo solo scrivere)

    //apertura del file in lettura
    FILE *in;
    if( (in = fopen(filename, "r")) == NULL ) err("fopen");

    char buff[BUFFSIZE];
    char found; //1 se ci sono tutte le parole, 0 altrimenti 

    mem->done = 0;

    while( fgets(buff, BUFFSIZE, in) ){

        if( buff[strlen(buff) -1]  == '\n' )
            buff[strlen(buff) -1] = '\0';
        //reset map dei processi W (imposta tutti i bit a 0)
        mem->map = 0; 
        strncpy(mem->line, buff, BUFFSIZE);
        //svegliamo tutti i processi W
        for(int i=0; i<word_num; i++) SIGNAL(semid, ACTIVE_W);
        //aspettiamo tutti i processi W
        for(int i=0; i<word_num; i++) WAIT(semid, R_SEM);

        //I processi W-i hanno terminato; possiamo vedere se tutte le parole sono presenti
        //all'interno della stringa line contenuta in mem (tramite array di mem)
        found = 1;
        for(int i=0; i<word_num; i++){
            if( ( mem->map & (int)pow(2.0, (double) i) ) == 0 ){
                found = 0;
                break;
            }
        }

        //se ci sono tutte le parole, manda la stringa a O
        if(found)
            if ( write(pipefd[1], buff, BUFFSIZE) < 0 ) err("write R");

    }

    //letto tutto il file: si terminano i processi W
    //i processi W saranno in attesa, quindi possiamo scrivere in mem
    mem->done = 1;
    for(int i=0; i<word_num; i++) SIGNAL(semid, ACTIVE_W);

    //interrompiamo il processo O
    char *stopmsg = STOPMSG;
    if ( write(pipefd[1], stopmsg, strlen(stopmsg)) < 0 ) err("write R");


    fclose(in);
    close(pipefd[1]);
    if( shmdt(mem) < 0 ) err("shmdt");
    exit(0);
}

void w_process(char *word, int index, int semid, int shmid){

    //area di memoria del processo
    shared_mem *mem;
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem*) -1 ) err("shmat W");

    while(1){
        //prima di entrare nell'area di memoria condivisa, effettuiamo un WAIT sul semaforo dei W
        WAIT(semid, ACTIVE_W);
        //leggiamo nell'area di memoria

        //se line è NULL, il processo R ha terminato
        if( mem->done) break;

        //se la parola associata al processo è presente 
        if ( strcasestr(mem->line, word) != NULL )
            mem->map += (int) pow( 2.0,  (double) index); 
        //risveglia il processo R
        SIGNAL(semid, R_SEM);
    }

    if( shmdt(mem) < 0 ) err("shmdt W");
    exit(0);
}

void o_process(int pipefd[2]){

    char buff[BUFFSIZE];
    close(pipefd[1]);
    char *stopmsg = STOPMSG;

    while(1){
        if ( read(pipefd[0], buff, BUFFSIZE) < 0 ) err("read O");

        if(  strncmp(buff, stopmsg, strlen(stopmsg)) == 0 )
            break;
        
        printf("%s\n\n", buff);
    }

    close(pipefd[0]);
    exit(0);
}