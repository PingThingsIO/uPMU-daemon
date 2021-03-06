/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * (C) 2015, 2016 Michael Andersen <m.andersen@cs.berkeley.edu>
 * (C) 2015, 2016 Sam Kumar <samkumar@berkeley.edu>
 * (C) 2015, 2016 Regents of the University of California
 */

#define EVENT_BUF_LEN 128 * ( sizeof (struct inotify_event) )
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define FULLPATHLEN 96 // the maximum length of a full file path
#define FILENAMELEN 48 // the maximum length of a file or directory name (within the root directory)
#define TIMEDELAY 10 // the number of seconds to wait between subsequent tries to reconnect
#define NUMFAILURES 360 // the number of failed connection attempts that will be tolerated before the program exits
#define MAXDEPTH 4 // the root directory is at depth 0
#define CHUNK_SIZE 31560 // the size of the portions into which each file is broken up
#define LASTFILEWAIT 240 // the number of seconds to wait before sending the last file when processing existing files
#define SOCKETTIMEOUT 600 // the number of seconds to wait for a send or receive operation on a socket before timing out


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <arpa/inet.h>

/* When my comments refer to the "root directory", they mean the directory the program is watching */

int ADDRESSP = 1883;

typedef struct 
{
    int fd;
    char path[FULLPATHLEN];
} watched_entry_t;

watched_entry_t children[MAXDEPTH + 1];

fd_set set;

// information about the root directory
int rootdirOpen = 0; // 1 if open (i.e., root directory needs to be closed)
char rootpath[FULLPATHLEN];

// when processing directories upon initialization, store the watched directories in an array
int num_watched_dirs = 0;
int size_watched_arr = 0;

// the socket descriptor
int socket_des = -1;

// the serial number of this uPMU
char* serialNum;
uint32_t size_serial;
uint32_t size_serial_word;

// 1 if connected (i.e., socket connection needs to be closed), 0 otherwise
int connected = 0;

// pointer to the server address
struct sockaddr* server_addr;

// the id of the next message sent to the server
uint32_t sendid = 1;

// the timeout for the socket
struct timeval socket_timeout;

/* Deletes a directory if possible, printing messages as necessary. */
void remove_dir(const char* dirpath)
{
    errno = 0;
    if (rmdir(dirpath) != 0)
    {
        if (errno == ENOTEMPTY || errno == EEXIST)
        {
            printf("Directory %s not removed: still contains files\n", dirpath);
        }
        else
        {
            printf("Directory %s not removed: no permissions OR directory in use\n", dirpath);
        }
    }
}

/* Finds the smallest int larger than the input that's a multiple of 4. */
uint32_t roundUp4(uint32_t input)
{
    return (input + 3) & 0xFFFFFFFCu;
}

/* Close the socket connection. */
void close_connection(int socket_descriptor)
{
    shutdown(socket_descriptor, SHUT_RDWR);
    close(socket_descriptor);
}

/* Exit, closing the socket connection if necessary. */
void safe_exit(int arg)
{
    printf("Exiting...\n");
    if (connected)
    {
        close_connection(socket_des);
    }
    fflush(stdout);
    exit(arg);
}

/* Attempts to connect to the server_addr and returns the socket descriptor
 * if successful. If not successful, return -1.
 */
int make_socket()
{
    printf("Attempting to connect...\n");
    int socket_descriptor = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_descriptor < 0)
    {
        perror("could not create socket");
        safe_exit(1);
    }
    if (connect(socket_descriptor, server_addr, sizeof(*server_addr)) < 0)
    {
        perror("could not connect");
        close(socket_descriptor);
        return -1;
    }
    errno = 0;
    setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(struct timeval));
    setsockopt(socket_descriptor, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(struct timeval));
    if (errno != 0) {
        perror("could not set socket timeout");
        close(socket_descriptor);
        return -1;
    }
    printf("Successfully connected\n");
    return socket_descriptor;
}

/* Sends the contents of the file at FILEPATH over TCP using SOCKET_DESCRIPTOR, if it
 * is a .dat file.
 * The total data sent is: 1. an id number, 2. the length of the filepath, 3.
 * the filepath, 4. the length of the contents of the file, and 5. the contents
 * of the file.
 * The filepath sent includes the filename itself, and contains the parent directory
 * if INROOTDIR is 1.
 * Returns 0 if the transmission was successful or if the file did not have to be sent.
 * Returns 1 if there was an error reading the file.
 * Returns -1 if the file was read properly but could not be sent.
 */
int send_file(int socket_descriptor, const char* filepath)
{    
    // Ignore files that are not .dat
    const char* substr = filepath + strlen(filepath) - 4;
    if (strcmp(substr, ".dat") != 0)
    {
        printf("Skipping file %s (not \".dat\")\n", filepath);
        return 0;
    }

    uint32_t size = strlen(filepath);
    
    // Open file
    FILE *input;
    input = fopen(filepath, "rb");
    if (input == NULL)
    {
        printf("Error: cannot read file %s.\n", filepath);
        perror("Details");
        return 1;
    }
    
    // Get the length of the file
    fseek(input, 0, SEEK_END);
    uint32_t length = ftell(input);
    
    // Store file number (sendid), length of serial number, length of filename, length of data, and filename in data array
    // The null terminator may be overwritten; the length of the filename does not
    // include the null terminator.
    // Space is added to the end of filename so it is word-aligned.
    uint32_t size_word = roundUp4(size); // size with extra bytes so it's word-aligned
    uint8_t data[16 + size_word + size_serial_word] __attribute__((aligned(4)));
    memset(data, 0, 16 + size_word + size_serial_word);
    *((uint32_t*) data) = sendid;
    *((uint32_t*) (data + 4)) = size;
    *((uint32_t*) (data + 8)) = size_serial;
    *((uint32_t*) (data + 12)) = length;
    strcpy((char*) (data + 16), filepath);
    strcpy((char*) (data + 16 + size_word), serialNum);
    rewind(input);
    
    // Send info
    int32_t dataleft = 16 + size_word + size_serial_word;
    int32_t datawritten;
    uint8_t* dest = data;
    while (dataleft > 0)
    {
        errno = 0;
        datawritten = write(socket_descriptor, dest, dataleft);
        if (datawritten < 0 || errno != 0)
        {
            printf("Could not send file %s\n", filepath);
            fclose(input);
            return -1;
        }
        dataleft -= datawritten;
        dest += datawritten;
    }
    // Send data
    uint8_t* tosend = malloc(CHUNK_SIZE);
    if (tosend == NULL) {
        printf("Could not allocate memory to store part of data file; try reducing CHUNK_SIZE.");
        safe_exit(1);
    }
    uint32_t totalread = 0;
    int32_t dataread;
    while (totalread != length)
    {
        dataread = fread(tosend, 1, CHUNK_SIZE, input);
        totalread += dataread;
        dataleft = dataread;
        dest = tosend;
        if (dataread != CHUNK_SIZE && totalread != length)
        {
            printf("Error: could not finish reading file %s (read %d out of %d bytes)\n", filepath, totalread, length);
            free(tosend);
            fclose(input);
            return 1;
        }
        while (dataleft > 0)
        {
            errno = 0;
            datawritten = write(socket_descriptor, dest, dataleft);
            if (datawritten < 0 || errno != 0)
            {
                printf("Could not send file %s\n", filepath);
                free(tosend);
                fclose(input);
                return -1;
            }
            dataleft -= datawritten;
            dest += datawritten;
        }
    }
    free(tosend);
    fclose(input);
    uint32_t response;
    
    // Get confirmation of receipt    
    dataleft = 4;
    while (dataleft > 0)
    {
        errno = 0;
        dataread = read(socket_descriptor, ((uint8_t*) &response) + 4 - dataleft, 4);
        if (dataread < 0 || errno != 0)
        {
            printf("Could not receive confirmation of receipt of %s\n", filepath);
            return -1;
        }
        else if (dataread == 0)
        {
            printf("Connection was closed before confirmation was received for %s\n", filepath);
            return -1;
        }
        dataleft -= dataread;
    }
    
    if (response != sendid++)
    {
        printf("Received improper confirmation of receipt of %s (will not be deleted)\n", filepath);
    }
    else
    {
        // Delete the file
        if (unlink(filepath) != 0)
        {
            printf("File %s was successfully sent and confirmation was received, but could not be deleted\n", filepath);
        }
    }
    if (sendid == 0xFFFFFFFFu)
    {
        sendid = 1;
    }
    sleep(1); // So that we don't use too much CPU time
    return 0;
}

/* Same as send_file(), except that it takes a pointer to the socket_descriptor instead
 * of the value itself, and that it will repeatedly try to send the file (waiting
 * TIMEDELAY seconds between attempts) if the file could be read but could not be sent
 * over TCP. It returns the value of the successful attempt (so it will never return -1,
 * just 0 or 1).
 */
int send_until_success(int* socket_descriptor, const char* filepath)
{
    int result;
    int numreconnects = -1;
    while ((result = send_file(*socket_descriptor, filepath)) == -1)
    {
        if (++numreconnects >= NUMFAILURES) {
            printf("Connection lost; failed to reconnect %d times. Exiting program.\n", numreconnects);
            safe_exit(1);
        }
        if (connected)
        {
            close_connection(*socket_descriptor);
            connected = 0;
        }
        sleep(TIMEDELAY);
        printf("Connection appears to be lost\n");
        *socket_descriptor = make_socket();
    }
    connected = 1;
    return result;
}

/* Used to compare two file entries so they can be sorted. */
int file_entry_comparator(const void* f1, const void* f2)
{
    return strcmp((const char*) f1, (const char*) f2);
}

/* Processes directory, sending files and adding watches (uses information in global variables) */
int processdir(const char* dirpath, int* socket_descriptor, int inotify_fd, int depth, int addwatchtosubs)
{
    if (strlen(dirpath) >= FULLPATHLEN - 5)
    {
        printf("%s too large: all filepaths must be less than %d characters long\n", dirpath, FULLPATHLEN);
        return -1;
    }
    DIR* rootdir = opendir(dirpath);
    if (rootdir == NULL)
    {
        printf("%s is not a valid directory\n", dirpath);
        return -1;
    }
    struct dirent* subdir;
    struct stat pathStats;
    int pathlen;
    char fullpath[FULLPATHLEN];
    int numsubdirs = 8; // Length of array in FILENAMELEN units
    int numfiles = 8; // Length of array in FILENAMELEN units
    char* filearr = malloc(numfiles * FILENAMELEN);
    unsigned int fileIndex = 0; // The number of strings actually in the array
    char* subdirarr = malloc(numsubdirs * FILENAMELEN);
    unsigned int subdirIndex = 0; // The number of strings actually in the array
    if (filearr == NULL || subdirarr == NULL)
    {
        printf("Not enough memory to store filenames or subdirectories to sort.\n");
        safe_exit(1);
    }
    // Add watch for root directory
    // Add files and directories to arrays
    while ((subdir = readdir(rootdir)) != NULL)
    {
        if (strcmp(subdir->d_name, ".") != 0 && strcmp(subdir->d_name, "..") != 0)
        {
            pathlen = strlen(dirpath) + strlen(subdir->d_name) + 2;
            if (pathlen > FULLPATHLEN)
            {
                printf("Filepath of length %d found; max allowed is %d\n", pathlen, FULLPATHLEN);
                free(filearr);
                free(subdirarr);
                return -1;
            }
            strcpy(fullpath, dirpath);
            strcat(fullpath, subdir->d_name);
            if (stat(fullpath, &pathStats) != 0)
            {
                printf("Could not read file %s\n", fullpath);
                continue;
            }
            if (S_ISDIR(pathStats.st_mode))
            {
                if (strlen(subdir->d_name) >= FILENAMELEN - 1)
                {
                    printf("Directory %s has name length of %d; max allowed is %d\n", subdir->d_name, (int) strlen(subdir->d_name), FILENAMELEN - 2);
                    free(filearr);
                    free(subdirarr);
                    return -1;
                }
                strcpy(subdirarr + (subdirIndex * FILENAMELEN), subdir->d_name);
                strcat(subdirarr + (subdirIndex * FILENAMELEN), "/");
                subdirIndex++;
                if (subdirIndex == numsubdirs)
                {
                    numsubdirs *= 2;
                    subdirarr = realloc(subdirarr, numsubdirs * FILENAMELEN);
                    if (subdirarr == NULL)
                    {
                        printf("Could not allocate memory to store subdirectory names to sort.\n");
                        safe_exit(1);
                    }
                }
            }
            else if (S_ISREG(pathStats.st_mode))
            {
                if (strlen(subdir->d_name) >= FILENAMELEN)
                {
                    printf("File %s has name length of %d; max allowed is %d\n", subdir->d_name, (int) strlen(subdir->d_name), FILENAMELEN - 1);
                    free(filearr);
                    free(subdirarr);
                    return -1;
                }
                strcpy(filearr + (fileIndex * FILENAMELEN), subdir->d_name);
                fileIndex++;
                if (fileIndex == numfiles)
                {
                    numfiles *= 2;
                    filearr = realloc(filearr, numfiles * FILENAMELEN);
                    if (filearr == NULL)
                    {
                        printf("Could not allocate memory to store filenames to sort.\n");
                        safe_exit(1);
                    }
                }
            }
        }
    }    
    closedir(rootdir);
    
    numfiles = fileIndex; // The actual number of files (so fileIndex can be changed)
    numsubdirs = subdirIndex; // The actual number of files (so subdirIndex can be changed)
    
    // Sort files and subdirectories in numerical order
    qsort(filearr, numfiles, FILENAMELEN, file_entry_comparator);
    qsort(subdirarr, numsubdirs, FILENAMELEN, file_entry_comparator);
    
    for (fileIndex = 0; fileIndex < numfiles; fileIndex++)
    {
        strcpy(fullpath, dirpath);
        strcat(fullpath, filearr + (fileIndex * FILENAMELEN));
        if (addwatchtosubs && (fileIndex == numfiles - 1))
        {
            printf("Waiting %d seconds for last file...\n", LASTFILEWAIT);
            sleep(LASTFILEWAIT);
        }
        send_until_success(socket_descriptor, fullpath);
    }
    
    free(filearr);
    
    int result;
    int addedwatch;
    // Process directories, adding watches
    for (subdirIndex = 0; subdirIndex < numsubdirs; subdirIndex++)
    {
        addedwatch = 0;
        strcpy(fullpath, dirpath);
        strcat(fullpath, subdirarr + (subdirIndex * FILENAMELEN));
        if (addwatchtosubs && (subdirIndex == (numsubdirs - 1)))
        {
            children[depth].fd = inotify_add_watch(inotify_fd, fullpath, IN_CREATE | IN_CLOSE_WRITE);
            strcpy(children[depth].path, fullpath);
            addedwatch = 1;
        }
        result = processdir(fullpath, socket_descriptor, inotify_fd, depth + 1, addwatchtosubs && (subdirIndex == (numsubdirs - 1)));
        if (result < 0)
        {
            free(subdirarr);
            return -1;
        }
        if (!addedwatch)
        {
            remove_dir(fullpath);
        }
    }
    free(subdirarr);
    return 0;
}

void interrupt_handler(int sig)
{
    safe_exit(0);
}

int main(int argc, char* argv[])
{
    struct rlimit memlimit;
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    getrlimit(RLIMIT_AS, &memlimit);
    memlimit.rlim_cur = (long) 400000000; // 4 MB
    memlimit.rlim_max = (long) 419430400; // 4 MiB
    setrlimit(RLIMIT_AS, &memlimit);
    if (argc != 4 && argc != 5)
    {
        printf("Usage: %s <directorytowatch> <targetserver> <uPMU serial number> [<port number>]\n", argv[0]);
        safe_exit(1);
    }
    
    socket_timeout.tv_sec = SOCKETTIMEOUT;
    socket_timeout.tv_usec = 0;

    int i, j;
    for (i = 0; i < MAXDEPTH; i++)
    {
        children[i].fd = -1;
    }
    
    serialNum = argv[3];
    size_serial = strlen(serialNum);
    size_serial_word = roundUp4(size_serial);
    
    if (argc == 5)
    {
        errno = 0;
        unsigned long port = strtoul(argv[4], NULL, 0);
        if (port > 65535 || port == 0 || errno != 0)
        {
            printf("Invalid port %s\n", argv[4]);
            safe_exit(1);
        }
        ADDRESSP = (int) port;
    }
    
    // Set up signal to handle Ctrl-C (close socket connection before terminating)
    struct sigaction interrupt_action;
    interrupt_action.sa_handler = interrupt_handler;
    interrupt_action.sa_flags = 0;
    sigemptyset(&interrupt_action.sa_mask);
    if (-1 == sigaction(SIGINT, &interrupt_action, NULL))
    {
        printf("Could not set up signal to handle keyboard interrupt\n");
        exit(1);
    }

    struct sigaction socket_action;
    socket_action.sa_handler = SIG_IGN;
    socket_action.sa_flags = 0;
    sigemptyset(&socket_action.sa_mask);
    if (-1 == sigaction(SIGPIPE, &socket_action, NULL))
    {
        printf("Could not set up signal to handle writing to broken socket\n");
        exit(1);
    }
    
    // Create the socket address
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(ADDRESSP);
    int result = inet_pton(AF_INET, argv[2], &server.sin_addr);
    if (result < 0)
    {
        perror("invalid address family in \"inet_pton\"");
        safe_exit(1);
    }
    else if (result == 0)
    {
        perror("invalid ip address in \"inet_pton\"");
        safe_exit(1);
    }
    server_addr = (struct sockaddr*) &server;
    connected = 0;
    socket_des = make_socket();
    int numreconnects = 0;
    while (socket_des == -1)
    {
        if (++numreconnects >= NUMFAILURES) {
            printf("Failed to connect %d times. Exiting program.\n", numreconnects);
            safe_exit(1);
        }
        sleep(TIMEDELAY);
        socket_des = make_socket();
    }
    connected = 1;
    
    // Look for file/directory additions
    int fd = inotify_init();
    if (fd < 0)
    {
        perror("inotify_init");
        safe_exit(1);
    }
    
    // This watch will notice any new files or subdirectories in the directory we are watching
    children[0].fd = inotify_add_watch(fd, argv[1], IN_CREATE | IN_CLOSE_WRITE);
    strcpy(children[0].path, argv[1]);
    if (argv[1][strlen(argv[1]) - 1] != '/')
    {
        strcat(children[0].path, "/");
    }
    if (processdir(children[0].path, &socket_des, fd, 1, 1) < 0)
    {
        printf("Could not finish processing existing files.\n");
        safe_exit(1);
    }
    printf("Finished processing existing files.\n");
    
    char buffer[EVENT_BUF_LEN];

    struct timeval timeout;
    int rlen;
    
    char fullname[FULLPATHLEN];

    int index;

    while (1)
    {
        //Select changes the timeout and the set
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeout.tv_sec = 2; //Do a wavelet keepalive every 3 seconds
        timeout.tv_usec = 0;
        rlen = select(fd + 1, &set, NULL, NULL, &timeout);
        if (rlen == 0)
        {
            // no activity
            continue;
        }
        rlen = read(fd, buffer, EVENT_BUF_LEN);
        index = 0;
        if (rlen == -1)
        {
            printf("Error (possibly caused by filepath that is too long)\n");
        }
        
        while (index < rlen)
        {
            result = 0;
            struct inotify_event* ev = (struct inotify_event*) &buffer[index];
            if (ev->len)
            {
                /* Check for a new directory */
                if ((IN_CREATE & ev->mask) && (IN_ISDIR & ev->mask))
                {
                    // Find the correct depth, store it in i
                    for (i = MAXDEPTH; i >= 0; i--)
                    {
                        if (children[i].fd == ev->wd)
                        {
                            break;
                        }
                    }
                    i++;
                    if (i > MAXDEPTH)
                    {
                        printf("WARNING: unexpected new directory past maximum depth (will be ignored)\n");
                    }
                    
                    char* parentname = children[i - 1].path;
                    strcpy(fullname, parentname);
                    strcat(fullname, ev->name);
                    strcat(fullname, "/");
                    if (strcmp(fullname, children[i].path) != 0) // check if we're already watching this (in case it was detected twice); if not, don't proceed
                    {
                        // remove watches from depth i and deeper and delete directories if possible
                        printf("Found new directory %s (depth %d)\n", fullname, i);
                        for (j = MAXDEPTH; j >= i; j--)
                        {
                            if (children[j].fd != -1) // if it has already been deleted or there's no watch, don't do anything
                            {
                                printf("Unwatching %s\n", children[j].path);
                                if (inotify_rm_watch(fd, children[j].fd))
                                {
                                    perror("RM watch");
                                }
                                else
                                {
                                    children[j].fd = -1; // mark it as deleted
                                }
                                remove_dir(children[j].path);
                            }
                        }
                        
                        strcpy(children[i].path, fullname);
                        printf("Watching %s\n", children[i].path);
                        children[i].fd = inotify_add_watch(fd, children[i].path, IN_CREATE | IN_CLOSE_WRITE);
                        // Ok great, but we may have missed some files, so let's check for them:
                        printf("Processing existing files in %s\n", fullname);
                        if (processdir(fullname, &socket_des, fd, i + 1, 1) < 0)
                        {
                            printf("WARNING: could not process existing files in newly created directory %s", fullname);
                        }
                        else
                        {
                            printf("Finished processing existing files in %s\n", fullname);
                        }
                    }
                    else
                    {
                        printf("Directory %s already found\n", fullname);
                    }
                }
                /* Check for a new file */
                else if (((IN_CLOSE_WRITE) & ev->mask) && !(IN_ISDIR & ev->mask))
                {
                    if (children[MAXDEPTH].fd == ev->wd)
                    {
                        strcpy(fullname, children[MAXDEPTH].path);
                        strcat(fullname, ev->name);
                        result = send_until_success(&socket_des, fullname);
                    }
                    else
                    {
                        printf("Warning: file %s appeared outside hour directory (not sent)\n", ev->name);
                    }
                }
                if (result == 1)
                {
                    printf("Could not read %s (file already sent, deleted concurrently, or not fully written)\n", ev->name);
                }
            }
            index += EVENT_SIZE + ev->len;
        }
    }
}
