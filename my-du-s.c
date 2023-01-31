#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<dirent.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/ipc.h>
#include<sys/sem.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<string.h>

#define MAX_PATH_LEN 16
#define MAX_PATHNAME_LEN 512

#define N_SEMS 2

//usato per l'accesso esclusivo all'area di memoria condivisa fra stater e scanner
#define MUTEX 0

//usato da stater per comunicare a scanner che ha termiato di leggere dall'area di memoria condivisa
#define STATER_READY_TO_READ 1

typedef struct{
    char pathname[MAX_PATHNAME_LEN];
    char file[MAX_PATHNAME_LEN];
    int ID; //definisce il processo che ha depositato il pathname: numerato da 0 ad n
}shm_mem;

typedef struct{
    long type;
    long int block_size;
    int ID;
}msgq_msg;

int WAIT(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,-1,0}};
    return semop(sem_des, operazioni, 1);
}
int SIGNAL(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,+1,0}};
    return semop(sem_des, operazioni, 1);
}

void err(char *err){
    perror(err);
    exit(1);
}

void stater_process(int pathnum, int shmid, int semid, int msgqid){

    shm_mem *mem;
    if((mem = (shm_mem*) shmat(shmid, NULL, 0)) == (shm_mem*) -1 ) err("shmat STATER");
    int ended_scanners = 0; //conta quanti processi scanner hanno terminato l'esecuzione
    struct stat statbuf;

    msgq_msg msg;
    msg.type = 1;

    while( 1 ){

        //se questo semaforo è a 1, possiamo leggere il contenuto della shared mem
        WAIT(semid, STATER_READY_TO_READ);

        //controlliamo se è un messaggio di interruzione
        if( mem->ID == -1 ){
            ended_scanners++;
            SIGNAL(semid, MUTEX);
            if( ended_scanners == pathnum) break;
            continue;
        }

        //change del path corrente per far funzionare la chiamata stat
        chdir(mem->pathname);

        if(stat(mem->file, &statbuf) < 0) err("stat STATER");

        printf("[STATER] %s => ISREG: %i, ISDIR: %i, BLOCKS: %li \n", mem->file, S_ISREG(statbuf.st_mode), S_ISDIR(statbuf.st_mode), statbuf.st_blocks);

        msg.block_size = statbuf.st_blocks;
        msg.ID = mem->ID;

        //inviamo al processo P il messaggio corretto
        if( msgsnd(msgqid, &msg, sizeof(msgq_msg) - sizeof(long), 0) < 0 ) err("msgsnd STATER");

        //una volta terminata la lettura possiamo risvegliare un processo scanner
        SIGNAL(semid, MUTEX);
    }

    //comunichiamo al processo padre che abbiamo finito di leggere tutte le directory
    msg.ID = -1;
    if( msgsnd(msgqid, &msg, sizeof(msgq_msg) - sizeof(long), 0) < 0 ) err("msgsnd STATER");


    if( shmdt(mem) < 0 ) err("shmdt STARTER");
    exit(0);
}

void scanner_process(int id, char *pathname, int depth, int shmid, int semid){

    shm_mem *mem;
    if((mem = (shm_mem*) shmat(shmid, NULL, 0)) == (shm_mem*) -1 ) err("shmat SCANNER");

    //cambiamo pathname per far funzionare la chiamata stat
    if( chdir(pathname) < 0 ) err("chdir SCANNER");

    struct dirent *curr; //file corrente
    struct stat statbuf;

    //apriamo la directory presente
    DIR *dir;
    if( (dir = opendir(pathname)) == NULL ) err("opendir SCANNER");

    while( (curr = readdir(dir)) ){

        //controllo per i puntatori alla directory padre ed alla directory root
        if( strncmp((char *)curr->d_name, "..", 2 ) == 0 || strncmp((char *)curr->d_name, ".", 1) == 0)
            continue;

        //printf("[SCANNER] %s => %s\n",pathname, curr->d_name);
        
        //stat della voce corrente
        if(stat( (char*) curr->d_name, &statbuf) < 0) err("stat SCANNER");

        //se è un file regolare mandiamo un messaggio al processo STATER tramite area condivisa
        if( S_ISREG(statbuf.st_mode)){

            //richiediamo l'accesso all'area di memoria condivisa
            WAIT(semid, MUTEX);

            mem->ID = id;
            strncpy(mem->pathname, (char*) pathname, MAX_PATHNAME_LEN);
            strncpy(mem->file, (char*) curr->d_name, MAX_PATHNAME_LEN);

            //risveglia il processo stater dato che adesso può leggere
            SIGNAL(semid, STATER_READY_TO_READ);

            //la signal corrispondente al semaforo MUTEX la effettuerà il processo STATER
        }

    }

    closedir(dir);

    //comunichiamo al processo stater che abbiamo terminato la lettura della directory
    WAIT(semid, MUTEX);
    mem->ID = -1;
    SIGNAL(semid, STATER_READY_TO_READ);

    if( shmdt(mem) < 0 ) err("shmdt SCANNER");
    exit(0);
}

//P
int main(int argc, char *argv[]){
    
    if(argc<2){
        fprintf(stderr, "utilizzo corretto: %s [path1, path2, ... , pathN] \n", argv[0]);
        exit(1);
    }

    //init area di memoria condivisa
    int shmid;
    if( (shmid = shmget(IPC_PRIVATE, sizeof(shm_mem), IPC_CREAT | 0660)) < 0 ) err("shmget P");
    shm_mem *mem;

    //init del pathname contenuto nella memoria condivisa
    //usato dal processo stater per controllare se l'area di memoria è vuota, in tal caso effettua una signal sul mutex
    if((mem = (shm_mem*) shmat(shmid, NULL, 0)) == (shm_mem*) -1 ) err("shmat P");
    for(int i=0; i<MAX_PATHNAME_LEN; i++)
        mem->pathname[i] = '\0';

    //init semaforo
    int semid; 
    if( (semid = semget(IPC_PRIVATE, N_SEMS, IPC_CREAT | 0660)) < 0 ) err("semget P");
    if( semctl(semid, MUTEX, SETVAL, 1) < 0 ) err("semctl P");
    if( semctl(semid, STATER_READY_TO_READ, SETVAL, 0) < 0 ) err("semctl P");

    //init msgqueue
    int msgqid;
    if( (msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0660)) < 0 ) err("msgget P");

    //CREAZIONE PROCESSI SCANNER
    for(int i=0; i<argc-1; i++){
        if(fork() == 0)
            scanner_process(i, argv[i+1], 0, shmid, semid);

    }

    //CREAZIONE PROCESSO STATER
    if(fork() == 0) stater_process(argc-1, shmid, semid, msgqid);

    //PROCESSO PADRE

    //array di block_num per ogni path
    long int block_size_arr[argc-1];
    for(int i =0; i<argc-1; i++) block_size_arr[i] = 0;

    msgq_msg msg;

    while(1){

        if( msgrcv(msgqid, &msg, sizeof(msgq_msg) - sizeof(long), 0, 0) < 0 ) err("msgrcv P");

        //controlliamo se è arrivato un messaggio di stop dal processo stater
        if( msg.ID == -1) break;

        block_size_arr[ msg.ID ] += msg.block_size;

    }

    //stampiamo tutte le dimensioni di tutti i path
    printf("\n");
    for(int i = 0; i<argc-1; i++){
        printf("%li\t%s\n", block_size_arr[i], argv[i+1]);
    }

    if( shmdt(mem) < 0 ) err("shmdt P");

    if( shmctl(shmid, IPC_RMID, NULL) < 0 ) err("shmctl DELETE P");
    if( semctl(semid, 0, IPC_RMID) < 0 ) err("semctl DELETE P");
    if( msgctl(msgqid, IPC_RMID, NULL) < 0 ) err("msgctl DELETE P");

    return 0;
}