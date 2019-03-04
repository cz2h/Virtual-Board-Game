#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXNAME 80  /* maximum permitted name size, not including \0 */
#define NPITS 6  /* number of pits on a side, not including the end pit */
#define NPEBBLES 4 /* initial number of pebbles per pit */
#define MAXMESSAGE (MAXNAME + 50) /* initial number of pebbles per pit */
#define GREETING "Welcome to Mancala. What is your name?"
#define INVALIDNAME "Please pick another name."

int port = 3000;
int listenfd;
int numplayer;

struct newin {
    int fd;
    struct newin* next;
    char name[MAXNAME];
};

struct player {
    int fd;
    char name[MAXNAME+1];
    int pits[NPITS+1];  // pits[0..NPITS-1] are the regular pits [0] [1] [2] [3] [4] [5] END: [6]
    // pits[NPITS] is the end m
    //other stuff undoubtedly needed here
    struct player *next;
};

struct newin *newplayer_fdset;
struct player *playerlist;
extern void parseargs(int argc, char **argv);
extern int makelistener();
extern int compute_average_pebbles();
extern int game_is_over();  /* boolean */
extern void broadcast(char *s);  /* you nwrite this one */
extern int accept_conneection(int fd); /* Accept connection and initialize username. */
extern void print_stat();
extern void delete_playerfd(int newplayerfd);
extern void fdwrite(int filefd, char *message1, char *message2, char *message3);
extern void announceall();
extern void fdwriteall(char* message1, char* message2, char* message3);

/* A helper function to return a pointer to the next player.
 */
struct player* nextplayer(struct player* currentplayer, int sendmsg) {
    struct player* next;
    struct player* current = playerlist;
    char name[MAXNAME + 18];
    if(currentplayer == NULL) {
        next = playerlist;
    } else if(currentplayer->next != NULL) {
        next = currentplayer->next;
    } else {
        next = playerlist;
    }
    if(next != NULL) {
        sprintf(name, "It is %s's move.\r\n", next->name);
    }
    while(current != NULL) {
        // Announce next player "Your move?"
        if(current->fd == next->fd && sendmsg) {
            if(write(current->fd, "Your move?\r\n", 
			    strlen("Your move?\r\n")) < 0) {
                perror("Next player announce");
            }
        } else if(current->fd != next->fd && sendmsg) {
            // Announce other player "Not your move."
            if(write(current->fd, name, strlen(name)) < 0) {
                perror("Other player announce");
            }
        }
        current = current->next;
    }

    return next;
}


/* A function to broadcast message to all players.
 */
void broadcast(char *s) {
    struct player *curplayer;
    if(playerlist != NULL) {
        curplayer = playerlist;
        while(curplayer != NULL) {
            if(curplayer->fd != 0) {
                if(write(curplayer->fd, s, strlen(s)) < 0) {
                    perror("Broadcast message");
                }
            }
            curplayer = curplayer->next;
        }
    }
}


/* Returns 1 + the first newline char or -1 if there is no newline char.
 */
int find_network_newline(const char *buf, int n) {
    // Case start with space or user input is nothing.
    if(strlen(buf) == 1) {
        return -1;
    }
    for(int i = 0; i < n; i++) {
        if(buf[i] == '\n') {
            return i + 1;
        }
    }

    return -1;
}


/* Read whatever user input and returns it.
 * Retruns -114514 iff uesr exits.
 */
int valid_move(int fd) {
    char user_input[MAXMESSAGE] = {'\0'};
    int num_read = read(fd, user_input, MAXMESSAGE);

    if(num_read == 0) {
        return -114514;
    }

    int move = strtol(user_input, NULL, 10);
    return move;
}


/* Helper to acquire full user name which ends with network newline.
 */
int readfullname(int client_fd, struct newin* unknownplayer) {
    // Number of chars had read so far.
    int inbuf = strlen(unknownplayer->name);
    int num_read, where;
    char buf[MAXNAME] = {'\0'};
	 
    // Case 1: Invalid input
    num_read = read(client_fd, &(buf[0]), MAXNAME);
    if(num_read + inbuf > MAXNAME) {
        (unknownplayer->name)[0] = '\0';
        return 2;
    }
    if(buf[0] == '\n') {
        return 2;
    } else if (num_read == 0) {
    //  Case 4: Player exits while entering name.
        return -1;
    }

    // Case 2: Success, find a new line char.
    if((where = find_network_newline(buf, strlen(buf) + 1)) > 0) {
        buf[where - 1] = '\0';
        strcat(unknownplayer->name, buf);
        return 0;
    }

    // Case 3: Read something but not complete player's name.
    strcat(unknownplayer->name, buf);
    return 1;
}



/* Returns 1 if there is no duplicate name, else 0.
 */
int no_duplicate_name(char *name) {
    struct player *current = playerlist;

    while(current != NULL) {
        if(strcmp(current->name, name) == 0) {
            return 0;
        }
        current = current->next;
    }

    return 1;
}


/* Helper to find a newin with client_fd
 */
struct newin* find_newin(int client_fd) {
    struct newin* current = newplayer_fdset;
    while(current->fd != client_fd) {
        current = current->next;
    }
    return current;
}


/* Create a new player iff successfully read the user name.
 */
int create_new_player(int client_fd) {
    int numpebbles = compute_average_pebbles();
    struct player* newplayer;
    struct newin* unknownplayer = find_newin(client_fd);

    if (client_fd < 0) {
        perror("server: accept");
        close(client_fd);
        exit(1);
    }
    int nametoolong = readfullname(client_fd, unknownplayer);
    // Case 1: Invalid user input.
    if(nametoolong == 2) {
		fdwrite(client_fd, INVALIDNAME, NULL, NULL);
        return -1;
    }
    // Case 3: Does not read newline char.
    if(nametoolong == 1) {
        return -1;
    }
    // Case 4: Player exits.
    if(nametoolong == -1) {
        return -2;
    }
    // Case 2: Success input.
    if(nametoolong == 0) {
        if(no_duplicate_name(unknownplayer->name)) {
            // Create new player if there is no duplicate user name.
            newplayer = malloc(sizeof(struct player));
            newplayer->next = playerlist;
            newplayer->fd = client_fd;
            strcpy(newplayer->name, unknownplayer->name);
            for(int i = 0; i < NPITS ; i++) {
                newplayer->pits[i] = numpebbles;
            }
            playerlist = newplayer;
            delete_playerfd(newplayer->fd);
            printf("Accepted connection from %s\n", unknownplayer->name);
			fdwriteall("Accepted connection from ", unknownplayer->name, NULL);
            return 0;
        } else {
            unknownplayer->name[0] = '\0';
            fdwrite(client_fd, INVALIDNAME, NULL, NULL);
            return -1;
        }
    }
    return -1;
}


/* A helper to delete a player from the linked list.
 */
void deleteplayer(int sockfd) {
    struct player *currentplayer = playerlist;
    char temp[MAXNAME] = {'\0'};
    while(currentplayer->fd != sockfd) {
        currentplayer = currentplayer->next;
    }

    strcpy(temp, currentplayer->name);
    strcat(temp, " exits\r\n");

    if(currentplayer == playerlist) {
        if(currentplayer->next == NULL) {
            free(playerlist);
            playerlist = NULL;
        } else {
            // At least two players.
            free(playerlist);
            playerlist = playerlist->next;
        }
    } else {
        currentplayer = playerlist;
        // Find the previous node of delted player.
        while(currentplayer->next->fd != sockfd) {
            currentplayer = currentplayer->next;
        }
        free(currentplayer->next);
        currentplayer->next = currentplayer->next->next;
    }
    broadcast(temp);
}


/* Helper to announce other player of current player's move.
 */
void move_announce(struct player* currentplayer, int move) {
    struct player* current = playerlist;
    char msg[MAXMESSAGE];

    while(current != NULL) {
        if(current != currentplayer) {
            sprintf(msg, "%s moves pit %d\r\n", currentplayer->name, move);
            if(write(current->fd, msg, strlen(msg)) < 0) {
                perror("Move announce to other player");
            }
        }
        current = current->next;
    }
}


/* Takes a pointer to a pointer of player, move the peddles according to the game's rule.
 */
int movepeddles(struct player** currentplayer, int index) {
    int num_to_move = (*currentplayer)->pits[index];
    int add = index;
    //Fail to move when pits[index] is 0
    if((*currentplayer)->pits[index] == 0) {
        return 1;
    }
    (*currentplayer)->pits[index] = 0;

    while(add < NPITS && num_to_move > 0) {
        add ++;
        (*currentplayer)->pits[add] += 1;
        num_to_move -= 1;
    }

    if(num_to_move != 0) {
        struct player* sibling = nextplayer(*currentplayer, 0);
        int i = 0;
        while(num_to_move > 0) {
            if(i != 6) {
                (sibling)->pits[i] += 1;
                num_to_move -=1;
                i += 1;
            } else {
                sibling = nextplayer(sibling, 0);
                i = 0;
            }
        }
    }
	
	announceall();
    print_stat();
    return 0;
}


/* Print the playrboards.
 */
void print_stat() {
    struct player* current = playerlist;
    char buf[MAXMESSAGE];
    char pits[MAXMESSAGE];
	printf("Current game status:\n");
    while(current != NULL) {
		sprintf(buf, "%s: ", current->name);
		for(int i = 0; i < NPITS + 1; i++){
			if(i == NPITS){
				sprintf(pits, "[end pit]%d", current->pits[i]);
			} else {
				sprintf(pits, "[%d]%d ", i, current->pits[i]);
			}
			strcat(buf, pits);
		}
        fprintf(stdout, "%s\n", buf);
        current = current->next;
    }
}


/* Helper function to remove player from fd_set and playerlist.
 */
void remove_player_fd(char* name, int player_fd, fd_set *all_fds) {
    deleteplayer(player_fd);
    FD_CLR(player_fd, all_fds);
}


/* A helper to detect any possible exit and qualified movements
 */
int makemove(struct player** currentplayer, fd_set *all_fds, fd_set *available_fds) {
    struct player* current = playerlist;
    int move = 99999;
    while(current != NULL) {
        int fd = current->fd;
        if(FD_ISSET(fd, available_fds)) {
            if(current == *currentplayer) {
                // Read from currentplayer and make movements
                while(move >= NPITS || move < 0) {
                    move = valid_move(current->fd);
                    // Case exits.
                    if(move == -114514) {
                        remove_player_fd(current->name, current->fd, all_fds);
                        *currentplayer = nextplayer(*currentplayer, 1);
                        return 1;
                    } else if (move >= NPITS || move < 0) {
					// Case uesr input invalid.
                        fdwrite(current->fd, "Please choose non empty pit.",
                                NULL, NULL);
                    }
                }
                // Move to next player if current player successfully do sth.
                if(movepeddles(&current, move) == 0) {
                    move_announce(*currentplayer, move);
                    *currentplayer = nextplayer(*currentplayer, 1);
                } else {
                    if(write((*currentplayer)->fd, "Please move non zero pit.\r\n",
                            strlen("Please move non zero pit.\r\n")) < 0) {
                        perror("Invalid move from currentplayer");
                    }
                }
            } else {
                // Check if some player exits.
                if((move = valid_move(current->fd) == -114514)) {
                    remove_player_fd(current->name, current->fd, all_fds);
                    return 1;
                }
                fdwrite(current->fd, "Not your move.", NULL, NULL);
            }
        }
        current = (current)->next;
    }
    return 0;
}


/* Helper to handle input from player when it is not their turn.
 */
void handle_noise(struct player* currentplayer, int client_fd) {

    if(currentplayer->fd != client_fd) {
        if(write(client_fd, "Not your move.\r\n", 
		    strlen("Not your move.\r\n")) < 0) {
            perror("Handle noise.");
        }
    }
}


/* Add a new playerfd into the waiting list.
 */
void add_waiting_player(int newplayerfd) {
    struct newin* playerfds = malloc(sizeof(struct newin));
    playerfds->fd = newplayerfd;
    playerfds->next = newplayer_fdset;
    newplayer_fdset = playerfds;
}

/* To delete a playerfd from newplayer_fdset
 */
void delete_playerfd(int newplayerfd) {
    struct newin* playerfds = newplayer_fdset;
    while(playerfds->fd != newplayerfd) {
        playerfds = playerfds->next;
    }
    if(playerfds == newplayer_fdset) {
        if(playerfds->next == NULL) {
            newplayer_fdset = NULL;
        } else {
            // At least two players.
            newplayer_fdset = newplayer_fdset->next;
        }
    } else {
        playerfds = newplayer_fdset;
        // Find the previous node of delted player.
        while(playerfds->next->fd != newplayerfd) {
            playerfds = playerfds->next;
        }
		struct newin* new_next = playerfds->next->next;
		playerfds->next = new_next;
	}
}

/* Returns a sock_fd that used to initialize a new player.
 */
int message_from_newplyaer(fd_set* listen_fds) {
    struct newin* fdset = newplayer_fdset;
    while(fdset != NULL) {
        if(FD_ISSET(fdset->fd, listen_fds)) {
            int res = fdset->fd;
            // delete_playerfd(res);
            return res;
        }
        fdset = fdset->next;
    }
    return 0;
}

/* Helper to announce current stat to new joined player.
 */
void announce(int filefd) {
    struct player* current = playerlist;
    char buf[MAXMESSAGE];
	char pits[MAXMESSAGE];
    while(current != NULL) {
		sprintf(buf, "%s: ", current->name);
		for(int i = 0; i < NPITS + 1; i++){
			if(i == NPITS){
				sprintf(pits, "[end pit]%d", current->pits[i]);
			} else {
				sprintf(pits, "[%d]%d ", i, current->pits[i]);
			}
			strcat(buf, pits);
		}
        strcat(buf, "\r\n");
        write(filefd, buf, strlen(buf));
        current = current->next;
    }
}

/* A helper function to send message to filefd using both msg1, msg2, msg3.
 */
void fdwrite(int filefd, char *message1, char *message2, char *message3) {
    char temp[MAXMESSAGE];
    sprintf(temp, "%s", message1);
    if(message2 != NULL) {
        strcat(temp, message2);
    }
    if(message3 != NULL) {
        strcat(temp, message3);
    }
    strcat(temp, "\r\n");
    if(write(filefd, temp, strlen(temp)) < 0) {
        perror("Helper: fdwrite");
    }
}


/* A helper to announce all players of current game status.
 */
void announceall() {
    struct player* current = playerlist;
    while(current != NULL) {
        announce(current->fd);
        current = current->next;
    }
}


/* The main function to run the game.
 */
int main(int argc, char **argv) {
    char msg[MAXMESSAGE];
    numplayer = 0;
    parseargs(argc, argv);
    int sock_fd = makelistener();
    int max_fd = sock_fd;
    fd_set all_fds, listen_fds;
    FD_ZERO(&all_fds);
    FD_SET(sock_fd, &all_fds);
    int firsttime = 1, newplayerfd;
    struct player* currentplayer;

    while (!game_is_over()) {
        listen_fds = all_fds;
        if(select(max_fd + 1, &listen_fds, NULL, NULL, NULL) <0) {
            perror("Select");
            exit(1);
        }
        // Whenever accepts, put newin into special newin.
        if(FD_ISSET(sock_fd, &listen_fds)) {
            newplayerfd = accept(sock_fd, NULL, NULL);
            fdwrite(newplayerfd, GREETING, NULL, NULL);
            FD_SET(newplayerfd, &all_fds);
            add_waiting_player(newplayerfd);
            if(newplayerfd > max_fd) {
                max_fd = newplayerfd;
            }
        }
        // Have a newin sending message.
        int newplayer_fd = message_from_newplyaer(&listen_fds);
        if(newplayer_fd) {
            int acceptres = create_new_player(newplayer_fd);
            // Case newin exits.
            if(acceptres == -2) {
                FD_CLR(newplayer_fd, &all_fds);
            } else if(acceptres == 0) {
                // Successfully create a new player.
                announce(newplayer_fd);
                if(firsttime || currentplayer == NULL) {
                    currentplayer = nextplayer(NULL, 1);
                    firsttime = 0;
                } else {
                    fdwrite(newplayer_fd, "It is ", currentplayer->name,"'s turn.");
                }
            }
        } else if(!firsttime) {
            makemove(&currentplayer, &all_fds, &listen_fds);
        }
    }
    broadcast("Game over!\r\n");
    printf("Game over!\n");

    for (struct player *p = playerlist; p; p = p->next) {
        int points = 0;
        for (int i = 0; i <= NPITS; i++) {
            points += p->pits[i];
        }
        printf("%s has %d points\r\n", p->name, points);
        snprintf(msg, MAXMESSAGE, "%s has %d points\r\n", p->name, points);
        broadcast(msg);
    }
    return 0;
}


void parseargs(int argc, char **argv) {
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            break;
        default:
            status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}


int makelistener() {
    struct sockaddr_in r;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *) &on, sizeof(on)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&r, sizeof(r))) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}


/* call this BEFORE linking the new player in to the list */
int compute_average_pebbles() {
    struct player *p;
    int i;

    if (playerlist == NULL) {
        return NPEBBLES;
    }

    int nplayers = 0, npebbles = 0;
    for (p = playerlist; p; p = p->next) {
        nplayers++;
        for (i = 0; i < NPITS; i++) {
            npebbles += p->pits[i];
        }
    }
    return ((npebbles - 1) / nplayers / NPITS + 1);  /* round up */
}


int game_is_over() { /* boolean */
    int i;

    if (!playerlist) {
        return 0;  /* we haven't even started yet! */
    }

    for (struct player *p = playerlist; p; p = p->next) {
        int is_all_empty = 1;
        for (i = 0; i < NPITS; i++) {
            if (p->pits[i]) {
                is_all_empty = 0;
            }
        }
        if (is_all_empty) {
            return 1;
        }
    }
    return 0;
}


/* A helper to announce all players of the messages.
 */
void fdwriteall(char* message1, char* message2, char* message3){
	struct player* current = playerlist;
	while(current != NULL){
		fdwrite(current->fd, message1, message2, message3);
		current = current->next;
	}
}