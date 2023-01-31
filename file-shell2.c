#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/ipc.h>
#include<sys/msg.h>
#include<sys/mman.h>
#include<dirent.h>
#include<string.h>
#include<wait.h>
#include<signal.h>

#define MAX_PARAMS 16 //sicurezza 
#define BUFFSIZE 1024

//operazioni del processo D-n
enum {NUM_FILES, TOTAL_SIZE, SEARCH_CHAR};

typedef struct{
    long type;
    int process_id;
    int op_type; //num_files, total_size o search_char
    char file_to_src[BUFFSIZE];
    char char_to_src;
}message;

void err(char *error){
    perror(error);
    exit(1);
}

//FUNZIONI UTILIZZATE DAI PROCESSI D (implementazioni sotto il main)
unsigned long files_op(char *pathname, int op_type);
unsigned long search_char(char *pathname, char* file_to_src, char char_to_src);


void d_process(long id, char *pathname, int msgqid){

    message msg;

    while( 1 ){

        //aspettiamo solo i messaggi spettanti a questo processo (messaggi con type=id)
        if( msgrcv(msgqid, &msg, sizeof(message) - sizeof(long), id, 0) < 0 ) err("msgrcv D");

        //controllo messaggio di uscita
        if( msg.op_type == -1)
            break;
        
        printf("\033[0;31m"); //colore rosso

        //ricevuto un messaggio: processiamo il comando
        if(msg.op_type == NUM_FILES)
            printf("%lu file\n", files_op(pathname, NUM_FILES));
        else if(msg.op_type == TOTAL_SIZE)
            printf("%lu bytes\n", files_op(pathname, TOTAL_SIZE) );
        else if(msg.op_type == SEARCH_CHAR){
            printf("%lu occurrences\n", search_char(pathname, msg.file_to_src, msg.char_to_src) );
        }

        printf("\033[0m"); //colore bianco
        
        //segnaliamo al processo padre che abbiamo terminato l'esecuzione del comando
        //il processo P aspetterà fino a quando non riceverà il segnale SIGUSR1
        kill(getppid(), SIGUSR1);
    }

    exit(0);
}

void sig_handler(int sig) {return;} //handler dei segnali (ignora il segnale)

int main(int argc, char *argv[]){

    if(argc < 2 || argc >= MAX_PARAMS+1 ){
        fprintf(stderr, "sintassi errata. utilizzo corretto: %s <directory-1> <directory-2> <...> (max 16 directories)\n",
            argv[0]);
        exit(1);
    }

    //registriamo l'handler per i segnali (ignoriamo i segnali che i processi D invieranno)
    signal(SIGUSR1, sig_handler);

    //PROCESSO P: INIT CODA DI MESSAGGI
    int msgqid;
    if( (msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0660 )) < 0 ) err("msgget P");

    //creiamo i processi D-n
    int process_num = argc-1;
    for(int id=1; id<=process_num; id++)
        if( fork()==0 )
            d_process( (long) id, argv[id], msgqid);

    
    //PROCESSO P
    char buff[BUFFSIZE];
    char *cmd;
    long process_id; //usato come type del messaggio

    //usati nel comando search_char
    char *file;
    char char_to_src; 

    message msg;

    while(1){

        printf("\033[0;32m"); //colore verde
        printf("file-shell> ");
        printf("\033[0m"); //colore bianco
        
        //attesa per l'input
        fgets(buff, BUFFSIZE, stdin);
        //in buff abbiamo il comando chiamato: controllo di correttezza
        cmd = strtok(buff, " ");

        //controlliamo se è un messaggio di stop
        if( strncmp(cmd, "exit", strlen("exit")) == 0 )
            break;

        //controlliamo il tipo di comando
        if( strncmp(cmd, "num_files", strlen("num_files")) == 0 )
            msg.op_type = NUM_FILES;
        else if( strncmp(cmd, "total_size", strlen("total_size")) == 0 )
            msg.op_type = TOTAL_SIZE;
        else if( strncmp(cmd, "search_char", strlen("search_char")) == 0 )
            msg.op_type = SEARCH_CHAR;
        else{
            printf("comando non riconosciuto.\n");
            continue;
        }

        //controlliamo a chi è diretto il messaggio
        process_id = atol(strtok(NULL, " "));
        if( process_id < 1 || process_id > process_num){
            printf("processo non riconosciuto.\n");
            continue;
        }
        msg.type = process_id;

        //se l'op è di tipo SEARCH CHAR, otteniamo le rimanenti informazioni (file e char da cercare)
        if( msg.op_type == SEARCH_CHAR){
            strncpy(msg.file_to_src, strtok(NULL, " "), BUFFSIZE);
            msg.char_to_src = strtok(NULL, " ")[0];
        }

        //abbiamo costruito il messaggio: mandiamolo al processo D
        if( msgsnd(msgqid, &msg, sizeof(message) - sizeof(long), 0) < 0 ) err("msgsnd P");

        //aspettiamo che il rispettivo processo abbia termiato l'esecuzione del comando
        //(aspettiamo l'invio del segnale SIGUSR1, se lo riceviamo lo ignoriamo)
        pause();
    }

    //messaggio di uscita per i processi
    msg.op_type = -1;
    for(int id=1; id<=process_num; id++){
        msg.type = id;
        if( msgsnd(msgqid, &msg, sizeof(message) - sizeof(long), 0) < 0 ) err("msgsnd P");
    }

    //aspettiamo i processi prima di eliminare la coda di messaggi
    for(int id=1; id<=process_num; id++)
        wait(NULL);

    if( msgctl(msgqid, IPC_RMID, NULL) < 0 ) err("msgctl P");
    return 0;

}//FINE MAIN

unsigned long files_op(char *pathname, int op_type){

    ulong stats = 0; //numero di file o dimensione in byte in base ad op_type

    DIR *dir;
    if( (dir = opendir(pathname)) == NULL) err("opendir");

    char old_working_dir[BUFFSIZE];
    //prediamo la directory di lavoro corrente
    getcwd(old_working_dir, BUFFSIZE);

    //settiamo la working dir al pathname passato (per far funzionare la chiamata stat)
    chdir(pathname);

    struct dirent *curr; //voce di directory corrente
    struct stat statbuf;

    while( (curr = readdir(dir)) ){

        //skippiamo i puntatori alla directory corrente e precedente
        if( strncmp(curr->d_name, "..", 2) == 0 || strncmp(curr->d_name, ".", 1) == 0 )
            continue;

        //controllo di regolarità
        if( stat(curr->d_name, &statbuf) < 0 ) err("stat");

        //se è un file regolare, operiamo in base ad op_type
        if( S_ISREG(statbuf.st_mode) ){
            if(op_type == NUM_FILES)
                stats++;
            else if(op_type == TOTAL_SIZE)
                stats += (ulong) statbuf.st_size;
        }

    }

    //impostiamo nuovamente la directory di lavoro originale
    chdir(old_working_dir);
    closedir(dir);

    return stats;

}

unsigned long search_char(char *pathname, char* file_to_src, char char_to_src){

    char old_working_dir[BUFFSIZE];
    //prediamo la directory di lavoro corrente
    getcwd(old_working_dir, BUFFSIZE);

    //settiamo la working dir al pathname passato (per poter aprire il file e prelevare stats per la mappatura)
    chdir(pathname);

    int fd;
    if( (fd = open(file_to_src, O_RDONLY)) < 0) err("open");

    struct stat statbuf;
    if( stat(file_to_src, &statbuf) < 0 ) err("stat");
    ulong file_dim = (ulong) statbuf.st_size;

    char *mapped_file;
    if( (mapped_file = (char *) mmap(NULL, file_dim, PROT_READ, MAP_PRIVATE, fd, 0)) == NULL) err("mmap");

    int char_num = file_dim/sizeof(char);

    ulong occurrences = 0;

    for(int i=0; i<char_num; i++)
        if( mapped_file[i] == char_to_src)
            occurrences++;

    //impostiamo nuovamente la directory di lavoro originale
    chdir(old_working_dir);

    if( munmap(mapped_file, file_dim) < 0 ) err("munmap");
    if( close(fd) < 0) err("close");

    return occurrences;

}