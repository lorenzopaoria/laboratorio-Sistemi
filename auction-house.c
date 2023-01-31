#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/sem.h>
#include<wait.h>
#include<string.h>
#include<time.h>
//semafori
#define BUFFSIZE 256
#define MAX_BIDDERS 32

#define N_SEMS 2
enum {B_MUTEX, J_MUTEX};

typedef struct{
    char obj[BUFFSIZE];
    int min_offer;
    int max_offer;
    int current_offer;
    int auction_num;
    int offer_maker; //ID di chi ha fatto l'offerta
    int offer_done[MAX_BIDDERS]; //per salvare chi ha già offerto nel turno corrente
}shared_mem;

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

void bidder_process(int id, int shmid, int semid){

    shared_mem *mem;
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0) ) == (shared_mem*) -1  ) err("shmat J");

    srand(getpid() * time(NULL)); //per modificare i numeri casuali in base all'ID del processo

    while(1){

        WAIT(semid, B_MUTEX);

        //controlliamo se dobbiamo interrompere il processo
        if(mem->auction_num == -1)
            break;

        //se abbiamo già fatto un'offerta, diamo il turno a qualcun altro
        if(mem->offer_done[id]){
            SIGNAL(semid, B_MUTEX);
            continue;
        }

        //possiamo fare un'offerta
        int offer = rand()%(mem->max_offer);

        printf("[B%i]: invio offerta di %i EUR per asta n.%i\n", id+1, offer, mem->auction_num);
        
        mem->current_offer = offer;
        mem->offer_maker = id;
        mem->offer_done[id] = 1;

        //comunichiamo al processo J che abbiamo fatto un'offerta
        SIGNAL(semid, J_MUTEX);
    }

    if( shmdt(mem) < 0 ) err("shmdt B");
    exit(0);
}


//PROCESSO JUDGE
int main(int argc, char *argv[]){

    if(argc!=3){
        fprintf(stderr, "uso errato. sintassi: %s <auction-file> <num-bidders>\n", argv[0]);
        exit(1);
    }

    //creazione area di memoria condivisa
    int shmid;
    if( (shmid = shmget(IPC_PRIVATE, sizeof(shared_mem), IPC_CREAT | 0660)) < 0 ) err("shmget J");
    shared_mem *mem;
    if( (mem = (shared_mem*) shmat(shmid, NULL, 0) ) == (shared_mem*) -1  ) err("shmat J");

    //creazione semafori
    int semid;
    if ( (semid = semget(IPC_CREAT, N_SEMS, IPC_CREAT | 0660) ) < 0 ) err("semget J");
    if( semctl(semid, B_MUTEX, SETVAL, 0) < 0 ) err("semctl J");
    if( semctl(semid, J_MUTEX, SETVAL, 1) < 0 ) err("semctl J");

    //creazione processi bidders
    int bidders_number = atoi(argv[argc-1]);
    if(bidders_number > MAX_BIDDERS){
        printf("massimo numero di bidders: %i\n", MAX_BIDDERS);
        exit(1);
    }

    for(int i=0; i<bidders_number; i++)
        if( fork() == 0 )
            bidder_process(i, shmid, semid);

    //PROCESSO J
    FILE *in;
    if( (in = fopen(argv[1], "r")) == NULL ) err("fopen J");

    char buff[BUFFSIZE];
    char obj[BUFFSIZE];
    int min_offer, max_offer;

    int auction_num = 0; //numero di asta corrente
    int auctions_won = 0; //numero di aste vinte da qualche bidder
    int total_EUR = 0; //totale raccolto
    //il numero di aste perse è auction_num - auctions_won

    int offers[bidders_number];

    int best_offer; //migliore offerta dell'asta corrente
    int bid_winner; //chi ha vinto l'asta corrente (id del processo)
    int valid_offers; //numero di offerte valide (>min_offer) dell'asta corrente

    while( fgets(buff, BUFFSIZE, in) ){

        strncpy(obj, strtok(buff, ","), BUFFSIZE);
        min_offer = atoi(strtok(NULL,","));
        max_offer = atoi(strtok(NULL,","));

        WAIT(semid, J_MUTEX);

        //incrementiamo il numero di asta
        auction_num++;

        printf("[J]: lancio asta n.%i per %s con offerta minima di %i EUR ed offerta massima di %i EUR\n", 
            auction_num, obj, min_offer, max_offer);

        //nuova offerta
        mem->auction_num = auction_num;
        mem->max_offer = max_offer;
        mem->min_offer = min_offer;
        for(int i=0; i<bidders_number; i++) mem->offer_done[i] = 0;
        strncpy(mem->obj, obj, BUFFSIZE);

        //aspettiamo che ogni bid venga ricevuta (abbiamo già svegliato un processo BIDDER prima)
        for(int i=0; i<bidders_number; i++){
            SIGNAL(semid, B_MUTEX);
            WAIT(semid, J_MUTEX);
            //bid ricevuta: processarla
            printf("[J]: ricevuta offerta da B%i\n", mem->offer_maker+1);
            offers[mem->offer_maker] = mem->current_offer;
            //risvegliamo un'altro processo bidder
        }

        //usciti dal for, J_MUTEX e B_MUTEX saranno a 0; bisogna resettare J_MUTEX per la prossima iter. del whileubu
        SIGNAL(semid, J_MUTEX);

        //processiamo i risultati dell'asta
        valid_offers = 0;
        best_offer = -1;
        for(int i=0; i<bidders_number; i++){
            
            //controlliamo se l'offerta è valida
            if(offers[i] > mem->min_offer) valid_offers++;
            else continue;

            //con la maggioranza stretta ci assicuriamo che in caso di pareggio vinca chi ha fatto l'offerta prima
            if(offers[i] > best_offer){
                best_offer = offers[i];
                bid_winner = i;
            }

        }

        //controlliamo se esistono offerte valide e, se si, chi ha vinto l'asta
        printf("[J]: l'asta n.%i per %s si è conclusa ",auction_num, obj);
        if(valid_offers>0){
            printf("con %i offerte valide su %i; il vincitore è B%i!\n", 
                valid_offers, bidders_number, bid_winner+1);
                total_EUR += best_offer;
                auctions_won++;
        }else{
            printf("senza alcuna offerta valida pertanto l'oggetto non risulta assegnato.\n");
        }

    }

    //comunichiamo i risultati di tutte le aste
    printf("[J]: sono state svolte %i aste di cui %i andate assegnate ed %i andate a vuoto; il totale raccolto è di %i EUR!\n", 
        auction_num, auctions_won, auction_num-auctions_won, total_EUR );

    //terminiamo gli altri processi
    mem->auction_num = -1;
    for(int i=0; i<bidders_number; i++) SIGNAL(semid, B_MUTEX);

    //attendiamo i processi prima di distruggere le strutture di IPC
    for(int i=0; i<bidders_number; i++) wait(NULL);

    fclose(in);
    if( shmdt(mem) < 0 ) err("shmdt J");
    if( shmctl(shmid, IPC_RMID, NULL) < 0 ) err("shmctl J");
    if( semctl(semid, 0, IPC_RMID, NULL) < 0 ) err("semctl J");
    return 0;

}