#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <string.h>

#define MAX_LEN 5

enum{MMNG,MADD,MSUB,MMUL};//il mutex di mng =0, add=1, sub=2,mul=3

void err(char* error){
    perror(error);
    exit(1);
}

typedef struct {
    int risultato;
    int operando;
    int fileterminato;
}shared_mem;

int WAIT(int sem_des, int num_semaforo){
struct sembuf operazioni[1] = {{num_semaforo,-1,0}};
return semop(sem_des, operazioni, 1);
}
int SIGNAL(int sem_des, int num_semaforo){
struct sembuf operazioni[1] = {{num_semaforo,+1,0}};
return semop(sem_des, operazioni, 1);
}

void mng(char* filename, int shmid, int semid){
   
    FILE* in; //apertura file
    if( (in = fopen(filename, "r")) == NULL ) err("MNG fopen");

    
    shared_mem* mem;//mappamento area di memoria condivisa
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem*) -1) err("MNG shmat");

    
    mem->risultato = 0;//inizializzazione memoria condivisa
    mem->fileterminato = 0;

    
    char buff[MAX_LEN];//lettura dal file
    int risultato;
    while( fgets(buff, MAX_LEN, in) ){
        char val_s[MAX_LEN];
        strncpy(val_s, buff + 1, MAX_LEN);

        int val = atoi(val_s); //operando
        char op = buff[0]; //operatore

        WAIT(semid, MMNG);
        risultato = mem->risultato;
        printf("MNG: risultato parziale: %i; letto: %s", risultato, buff);

        mem->operando = val;

        if(op == '+') SIGNAL(semid, MADD);
        if(op == '*') SIGNAL(semid, MMUL);
        if(op == '-') SIGNAL(semid, MSUB);

        //WAIT(semid, MMNG);
    }
    WAIT(semid, MMNG);
    mem->fileterminato = 1;
    risultato = mem->risultato;

    printf("MNG: risultato finale: %i\n", risultato);
    
    fclose(in);

    exit(0);
}


void add(int shmid, int semaforoId){
    shared_mem* mem; //mappamento area di memoria condivisa
    
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem*) -1 ) err("ADD shmat");

    int val1;
    int val2;

    WAIT(semaforoId,MADD);
    while(!mem->fileterminato){
        val1 = mem->risultato;
        val2 = mem->operando;
        int risultato = val1 + val2;

        mem->risultato = risultato;
        printf("ADD: %i + %i = %i\n", val1, val2, risultato);

        SIGNAL(semaforoId, MMNG);
        WAIT(semaforoId, MADD);
    }
    exit(0);
}

void mul(int shmid, int semaforoId){
    shared_mem* mem;//mappamento area di memoria condivisa
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem*) -1 ) err("MUL shmat");

    int val1;
    long val2;

    WAIT(semaforoId, MMUL);
    while(!mem->fileterminato){
        val1 = mem->risultato;
        val2 = mem->operando;
        int risultato = val1 * val2;

        mem->risultato = risultato;
        printf("MUL: %i * %i = %i\n", val1, val2, risultato);

        SIGNAL(semaforoId, MMNG);
        WAIT(semaforoId, MMUL);
    }

    exit(0);
}

void sub(int shmid, int semaforoId){
    
    shared_mem* mem;//mappamento area di memoria condivisa
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0)) == (shared_mem*) -1 ) err("SUB shmat");

    int val1;
    long val2;

    WAIT(semaforoId, MSUB);
    while(!mem->fileterminato){
        val1 = mem->risultato;
        val2 = mem->operando;
        int risultato = val1 - val2;

        mem->risultato = risultato;
        printf("SUB: %i - %i = %i\n", val1, val2, risultato);

        SIGNAL(semaforoId, MMNG);
        WAIT(semaforoId, MSUB);
    }

    exit(0);
}

//P
int main(int argc, char *argv[]){

    if(argc != 2){
        fprintf(stderr, "uso non valido.\nsintassi corretta: %s <list>", argv[0]);
        exit(1);
    }

    //creazione segmento di memoria condiviso
    int shmid;
    if( ( shmid = shmget(IPC_PRIVATE, sizeof(shared_mem), IPC_CREAT | 0660) ) == -1) err("shmget P");

    //creazione semafori
    int semid;
    if( (semid = semget(IPC_PRIVATE, sizeof(shared_mem), IPC_CREAT | 0660)) == -1) err("semget P");
    if( semctl(semid, MMNG, SETVAL, 1) < 0 ) err("setting M_MNG value");
    for(int i = MADD; i <= MSUB; i++) if( semctl(semid, i, SETVAL, 0) < 0 ) err("setting M_OP value");

    //MNG
    if(!fork()) mng(argv[1], shmid, semid);

    //ADD
    if(!fork()) add(shmid, semid);

    //MUL
    if(!fork()) mul(shmid, semid);

    //SUB
    if(!fork()) sub(shmid, semid);

    wait(NULL);

    //termino tutti i processi OP
    for(int i = MADD; i <= MSUB; i++){
        SIGNAL(semid, i);
        wait(NULL);
    }

    if( (shmctl(shmid, IPC_RMID, NULL)) < 0 ) err("shm remove");
    if( (semctl(semid, 0, IPC_RMID, NULL)) < 0 ) err("sem remove");

    return 0;
}




