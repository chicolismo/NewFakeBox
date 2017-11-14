#ifndef __DROPBOX_CLIENT_H__
#define __DROPBOX_CLIENT_H__

#include <string>

#define CONNECTION_SUCCESS = 0
#define CONNECTION_ERROR = (-1)

enum ConnectionResult { Success, Error };

void print_interface();
void run_interface();

void create_sync_dir();

void list_local_files();
void list_server_files();

ConnectionResult connect_server(std::string host, uint16_t port);
void sync_client();
void send_file(std::string filename);
void get_file(std::string filename);
void delete_file(std::string filename);

void close_connection();

#endif
