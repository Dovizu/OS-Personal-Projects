#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>
#include <sys/select.h>

#include "libhttp.h"

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int server_port;
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;

int asprintf(char **strp, const char *fmt, ...);

char *read_file(char *path, size_t *file_size) {
  long size;
  char *file_contents;
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "Can't open the specified file %s\n", path);
    exit(1);
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  rewind(file);
  file_contents = malloc(size * (sizeof(char)));
  fread(file_contents, sizeof(char), size, file);
  fclose(file);
  if (file_size) *file_size = (size_t)size;
  return file_contents;
}

void respond_with_404(int fd) {
  http_start_response(fd, 404);
  http_send_header(fd, "Content-type", "text/html");
  http_end_headers(fd);
  http_send_string(fd, "<center><h1>404</h1><hr><p>Not Found.</p></center>");
}

void respond_with_400(int fd) {
  http_start_response(fd, 400);
  http_send_header(fd, "Content-type", "text/html");
  http_end_headers(fd);
  http_send_string(fd, "<center><h1>400</h1><hr><p>Bad Request.</p></center>");
}

void respond_with_directory(int fd, char *path) {
  DIR *dp;
  struct dirent *ep;
  dp = opendir(path);
  
  if (dp != NULL) {
    // de-resolve the path
    if (path[0] == '.' && path[1] == '/') path = path + 1;

    http_start_response(fd, 200);
    http_send_header(fd, "Content-type", "text/html");
    http_end_headers(fd);

    // build header
    char *header;
    asprintf(&header, "<h1>Index of %s</h1><hr>", path);
    http_send_string(fd, header);
    free(header);

    while ((ep = readdir(dp))) {
      if (strcmp(ep->d_name, ".") != 0) {  
        char *link_item;
        if (strcmp(ep->d_name, "..") == 0) {
          asprintf(&link_item, "<a href=\"../\">Parent directory</a><br>", path);
          http_send_string(fd, link_item);
        } else {
          asprintf(&link_item, "<a href=\"%s/%s\">%s</a><br>", path, ep->d_name, ep->d_name);
          http_send_string(fd, link_item);
        }
        free(link_item);
      }
    }
    closedir(dp);
  }
}

/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 */
void handle_files_request(int fd)
{

  /* YOUR CODE HERE */
  struct http_request *request = http_request_parse(fd);
  
  /* http requests must start with a leading slash */
  if (request->path[0] != '/') {
    respond_with_400(fd);
    exit(1);
  }
  /* resolve path */
  char *resolved_path;
  asprintf(&resolved_path, ".%s", request->path);
  request->path = resolved_path;
  /* determine file status */
  struct stat sb;
  /* Not found for various reasons */
  if (stat(request->path, &sb) == -1) {
    respond_with_404(fd);
    exit(EXIT_SUCCESS);
  }
  /* requested path is a regular file */
  if (S_ISREG(sb.st_mode)) {
    char *mime_type = http_get_mime_type(request->path);
    size_t file_size;
    char *file_content = read_file(request->path, &file_size);

    http_start_response(fd, 200);
    http_send_header(fd, "Content-type", mime_type);
    http_end_headers(fd);
    http_send_data(fd, file_content, file_size);
  }

  /* requested path is a directory */
  if (S_ISDIR(sb.st_mode)) {
    char *path_with_index_html;
    asprintf(&path_with_index_html, "%s/index.html", request->path);

    if (access(path_with_index_html, F_OK) != -1) {
      // index.html exists
      char *file_content = read_file(path_with_index_html, NULL);
      http_start_response(fd, 200);
      http_send_header(fd, "Content-type", "text/html");
      http_end_headers(fd);
      http_send_string(fd, file_content);
    } else {
      // return directory in links
      respond_with_directory(fd, request->path);
    }
    free(path_with_index_html);
  }
  free(resolved_path);
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target. HTTP requests from the client (fd) should be sent to the
 * proxy target, and HTTP responses from the proxy target should be sent to
 * the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void* thread_handle_incoming(void *);
void* thread_handle_outgoing(void *);
struct endpoints {
  /* data */
  int server_socket_number;
  int fd;
};

void handle_proxy_request(int fd)
{
  char *hostname = server_proxy_hostname;
  int port = server_proxy_port;
  struct hostent *server;
  int socket_number;
  struct sockaddr_in serv_addr;

  // get ip address
  if ((server = gethostbyname(hostname)) == NULL) {
    fprintf(stderr, "ERROR: no such host\n");
    exit(0);
  }
  // open socket
  socket_number = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_number < 0) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *) server->h_addr, 
    (char *) &serv_addr.sin_addr.s_addr, 
    server->h_length);
  serv_addr.sin_port = htons(port);

  if (connect(socket_number, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    fprintf(stderr, "ERROR connecting\n");
  }

  fd_set rwfds;
  struct timeval tv;
  int retval;
  int maxp;
  if (socket_number > fd) {
    maxp = socket_number + 1;
  } else {
    maxp = fd + 1;
  }
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  char buffer[256];
  while (1) {
    FD_ZERO(&rwfds);
    FD_SET(fd, &rwfds);
    FD_SET(socket_number, &rwfds);

    retval = select(maxp, &rwfds, NULL, NULL, &tv);
    if (retval == 0) {
      printf("Request timeout\n");
    } else if (FD_ISSET(fd, &rwfds)) {
      read(fd, buffer, 225);
      write(socket_number, buffer, strlen(buffer));
    } else if (FD_ISSET(socket_number, &rwfds)) {
      read(socket_number, buffer, 225);
      write(fd, buffer, strlen(buffer));
    }
  }
  
  // pthread_t incoming, outgoing;
  // struct endpoints args;
  // args.server_socket_number = socket_number;

  // pthread_create(&outgoing, NULL, thread_handle_outgoing, &args);
  // pthread_create(&incoming, NULL, thread_handle_incoming, &args);
  // pthread_join(incoming, NULL);
  // pthread_join(outgoing, NULL);
  // exit(1);
}

void* thread_handle_incoming(void *argument) {
  int ssn = ((struct endpoints *) argument)->server_socket_number;
  int fd = ((struct endpoints *) argument)->fd;
  char buffer[256];
  int n;
  while(1) {
    n = read(ssn, buffer, 225);
    if (n < 0) {
      printf("Error reading from ssn\n");
      exit(errno);
    }
    printf("received from server: %s\n", buffer);
    n = write(fd, buffer, strlen(buffer));
    if (n < 0) {
      printf("Error writing to fd\n");
      exit(errno);
    }
    bzero(buffer, 256);
  }
}

void* thread_handle_outgoing(void *argument) {
  int ssn = ((struct endpoints *) argument)->server_socket_number;
  int fd = ((struct endpoints *) argument)->fd;
  char buffer[256];
  int n;
  while(1) {
    n = read(fd, buffer, 225);
    if (n < 0) {
      printf("Error reading from fd\n");
      exit(errno);
    }
    printf("received from client: %s\n", buffer);
    n = write(ssn, buffer, strlen(buffer));
    if (n < 0) {
      printf("Error writing to ssn\n");
      exit(errno);
    }
    bzero(buffer, 256);
  }
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int* socket_number, void (*request_handler)(int))
{

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;
  pid_t pid;

  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    fprintf(stderr, "Failed to set socket options: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  if (bind(*socket_number, (struct sockaddr*) &server_address,
        sizeof(server_address)) == -1) {
    fprintf(stderr, "Failed to bind on socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
    fprintf(stderr, "Failed to listen on socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

  while (1) {

    client_socket_number = accept(*socket_number, (struct sockaddr*) &client_address,
        (socklen_t*) &client_address_length);
    if (client_socket_number < 0) {
      fprintf(stderr, "Error accepting socket: error %d: %s\n", errno, strerror(errno));
      continue;
    }

    printf("Accepted connection from %s on port %d\n", inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

    pid = fork();
    if (pid > 0) {
      close(client_socket_number);
    } else if (pid == 0) {
      signal(SIGINT, SIG_DFL); // Un-register signal handler (only parent should have it)
      close(*socket_number);
      request_handler(client_socket_number);
      close(client_socket_number);
      exit(EXIT_SUCCESS);
    } else {
      fprintf(stderr, "Failed to fork child: error %d: %s\n", errno, strerror(errno));
      exit(errno);
    }
  }

  close(*socket_number);

}

int server_fd;
void signal_callback_handler(int signum)
{
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(signum);
}

char *USAGE = "Usage: ./httpserver --files www_directory/ --port 8000\n"
              "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{

  signal(SIGINT, signal_callback_handler);

  /* Default settings */
  server_port = 8000;
  server_files_directory = malloc(1024);
  getcwd(server_files_directory, 1024);
  server_proxy_hostname = "inst.eecs.berkeley.edu";
  server_proxy_port = 80;

  void (*request_handler)(int) = handle_files_request;

  int i;
  for (i = 1; i < argc; i++)
  {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;

}
