#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/shm.h>
#include<sys/sem.h>
#include<sys/wait.h>
#include <ctype.h>
#include<string.h>

#define MAX_LEN 1024

#define N_SEMS 2
#define PARENT_CONTINUE 0 //usato per coordinare l'accesso all'area della shm del Padre
#define ACTIVE_FILTERS 1 //usato per coordinare i processi FILTER

//id processi FILTER
#define UPPER 0
#define LOWER 1
#define REPLACE 2

int WAIT(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,-1,0}};
    return semop(sem_des, operazioni, 1);
}

int SIGNAL(int sem_des, int num_semaforo){
    struct sembuf operazioni[1] = {{num_semaforo,+1,0}};
    return semop(sem_des, operazioni, 1);
}


void err(char *error){
    perror(error);
    exit(1);
}

typedef struct{
    char line[MAX_LEN];
    int turn;
    int line_done; //1 se il filter n-esimo ha terminato la lettura
}shared_mem;

void modify_case(char *line, int type, char *specs){

    if( strstr(line, specs) == NULL) return;

    int occurrences;
    int speclen = strlen(specs);

    //check di spec all'interno della riga + modifica
    for(int i=0; i<MAX_LEN-speclen; i++){
        occurrences = 0;
        for(int j=0; j<speclen; j++){
            if(line[i+j] == specs[j]) occurrences++;
            else break;
            if( occurrences == speclen ){
                for(int k=0; k < speclen; k++)
                    line[k+i] = ( (type==UPPER)? toupper(line[k+i]) : tolower(line[k+i]) );
            }
        }
    }

}

void filter(int turn, int process_type, int filter_num, char *specs, int shmid, int semid){

    shared_mem *mem;
    if( (mem = shmat(shmid, NULL, 0)) == NULL ) err("shmat P");

    //per il processo di tipo REPLACE
    char *to_find;
    char *to_replace;
    if(process_type == REPLACE){
        to_find = strtok(specs, ",");
        to_replace = strtok(NULL, ",");
    }


    while(1){
        
        //aspetta il proprio turno
        WAIT(semid, ACTIVE_FILTERS);

        //controlliamo se dobbiamo interrompere il processo
        if(mem->turn == -1)
            break;

        //controllo del turno; se è sbagliato, diamo il turno a qualche altro processo S
        if( mem->turn != turn){
            SIGNAL(semid, ACTIVE_FILTERS);
            continue;
        }

        //turno corretto: effettuiamo le modifiche in base al process_type
        if(process_type == UPPER){
            modify_case(mem->line, UPPER, specs);
        }else if(process_type == LOWER){
            modify_case(mem->line, LOWER, specs);
        }else{//REPLACE

            char new_line[MAX_LEN];
            strncpy(new_line, mem->line, MAX_LEN);
            char *occurrence;

            while(  (occurrence = strstr(mem->line, to_find)) ){
                strncpy(new_line, mem->line, occurrence - mem->line);
                new_line[occurrence - mem->line] = '\0';
                strcat(new_line, to_replace);
                strcat(new_line, occurrence+strlen(to_replace));
                //modifichiamo la stringa originale
                strncpy(mem->line, new_line, MAX_LEN);
            }
            
        }

        //abbiamo modificato la stringa; passiamo il controllo al prossimo filtro
        mem->turn = turn+1; 

        //se abbiamo terminato di modificare la riga (siamo arrivati al filter n-esimo) notifichiamo il processo padre
        if(mem->turn == filter_num){
            mem->line_done = 1;
            SIGNAL(semid, PARENT_CONTINUE);
        }else{
            //sennò diamo il turno al processo corretto
            SIGNAL(semid, ACTIVE_FILTERS);
        }    
    }

    exit(0);
}

int main(int argc, char *argv[]){

    if(argc <= 2){
        printf("sintassi errata, utilizzo: %s <file.txt> <filter-1> [filter-2] [...]\n", argv[0]);
        exit(1);
    }

    //creazione ed inizializzazione memoria condivisa
    int shmid;
    if(  (shmid = shmget(IPC_PRIVATE, sizeof(shared_mem), IPC_CREAT | 0660 )) < 0 ) err("shmget P");
    shared_mem *mem;
    if( (mem = shmat(shmid, NULL, 0)) == NULL ) err("shmat P");
    mem->line_done = 0;

    //creazione semafori
    int semid;
    if( (semid = semget(IPC_PRIVATE, N_SEMS, IPC_CREAT | 0660 )) < 0 ) err("semget P");
    if( semctl(semid, PARENT_CONTINUE, SETVAL, 1)  < 0 ) err("semctl P");
    if( semctl(semid, ACTIVE_FILTERS, SETVAL, 0)  < 0 ) err("semctl P");

    int filter_num = argc - 2; //numero di filtri attivi
    int process_type;
    char specs[MAX_LEN]; //specifiche del filtro

    //creazione processi
    for(int i=0; i<filter_num; i++){

        //1. capire che tipo di filtro è
        if( argv[i+2][0] == '^')
            process_type = UPPER;
        else if( argv[i+2][0] == '_' )
            process_type = LOWER;
        else
            process_type = REPLACE;

        //2. sottostringa dal secondo carattere fino alla fine
        strncpy(specs, (argv[i+2])+1, strlen(argv[i+2]));

        //creazione processo
        if( fork() == 0 ) filter(i, process_type, filter_num, specs, shmid, semid);

    }

    //PROCESSO PADRE
    FILE *in;
    if( (in = fopen(argv[1], "r")) == NULL ) err("fopen P");

    char buff[MAX_LEN];
    
    while( fgets(buff, MAX_LEN, in) ){
        
        //annuncia che il padre sta per leggere ed entrare nella sua area di memoria condivisa
        WAIT(semid, PARENT_CONTINUE);

        //se i processi hanno processato la linea, stampa
        if(mem->line_done)
            printf("%s", mem->line);

        strncpy(mem->line, buff, MAX_LEN);
        mem->turn = 0;
        mem->line_done = 0;
        SIGNAL(semid, ACTIVE_FILTERS); //attiviamo i processi filtro

        //la SIGNAL al semaforo PARENT_CONTINUE verrà fatta dal processo filter-n
    }

    //print dell'ultima riga
    printf("%s\n", mem->line);

    //interrompiamo tutti i processi filtro
    mem->turn = -1;
    for(int i=0; i<filter_num; i++) SIGNAL(semid, ACTIVE_FILTERS);

    //aspettiamo i processi prima di eliminare le strutture di IPC
    for(int i=0; i<filter_num; i++) wait(NULL);

    if( fclose(in) < 0 ) err("fclose P");
    if( shmctl(shmid, IPC_RMID, NULL) < 0 ) err("shmctl P");
    if( semctl(semid, 0, IPC_RMID, NULL) < 0 ) err("semctl P");
    return 0;

}