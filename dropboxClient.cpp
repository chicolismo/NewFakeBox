#include <netinet/in.h>
#include "dropboxClient.h"
#include "dropboxUtil.h"
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <strings.h>
#include "dropboxServer.h"
#include <boost/filesystem.hpp>
#include <sstream>
#include <thread>
#include <netdb.h>

namespace fs = boost::filesystem;

std::mutex socket_mutex;

// O id do usuário
std::string user_id;

uint16_t port_number;

// O endereço do servidor
sockaddr_in server_address{};

// Socket para se comunicar com o servidor.
int socket_fd;

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Argumentos insuficientes\n";
        std::cerr << "./client <user_id> <hostname> <port_number>";
        std::exit(1);
    }

    user_id = std::string(argv[1]);
    std::string hostname = std::string(argv[2]);
    char *end;
    port_number = static_cast<uint16_t>(std::strtol(argv[3], &end, 10));

    if (connect_server(hostname, port_number) == ConnectionResult::Error) {
        std::cerr << "Erro ao se conectar com o servidor\n";
        std::exit(1);
    }

    //sync_client();

    // Exibe a interface
    run_interface();
}

ConnectionResult connect_server(std::string hostname, uint16_t port) {
    hostent *server = gethostbyname(hostname.c_str());

    if (server == nullptr) {
        std::cerr << "Erro ao obter o servidor\n";
        return ConnectionResult::Error;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "Erro ao criar o socket do cliente\n";
        return ConnectionResult::Error;
    }

    bzero((void *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr = *(in_addr *) server->h_addr_list[0];

    std::cout << "Tentando se conecar com o servidor (UserId: " << user_id << ")\n";
    if (connect(socket_fd, (sockaddr *) &server_address, sizeof(server_address)) < 0) {
        std::cerr << "Erro ao conectar com o servidor\n";
        return ConnectionResult::Error;
    }

    // Envia o tipo de conexão ao servidor
    ConnectionType type = ConnectionType::Normal;
    ssize_t bytes = write_socket(socket_fd, (const void *) &type, sizeof(type));
    if (bytes == -1) {
        std::cerr << "Erro enviando o tipo de conexão ao servidor";
        return ConnectionResult::Error;
    }

    /*
    size_t size = user_id.size() + 1;
    char buffer[size];
    bzero((void *) buffer, size);
    std::strcpy(buffer, user_id.c_str());
    buffer[size] = '\0';
    // Envia o tamanho do user_id
    write_socket(socket_fd, (const void *) &size, sizeof(size));
    // Envia o user_id
    write_socket(socket_fd, (const void *) buffer, size);
     */

    send_string(socket_fd, user_id);

    // Recebe o sinal de ok do servidor
    bool ok = false;
    read_socket(socket_fd, (void *) &ok, sizeof(ok));

    if (!ok) {
        return ConnectionResult::Error;
    }

    return ConnectionResult::Success;
}


void print_interface() {
    std::cout << "Digite o comando:\n";
    std::cout << "\tupload <path/filename.ext>\n";
    std::cout << "\tdownload <filename.ext>\n";
    std::cout << "\tdelete <filename.ext>\n";
    std::cout << "\tlist_server\n";
    std::cout << "\tlist_client\n";
    std::cout << "\tget_sync_dir\n";
    std::cout << "\texit\n";
}


void run_interface() {
    std::string delim(" ");
    std::string input;
    std::string command;
    std::string argument;

    do {
        print_interface();
        std::getline(std::cin, input);
        command = input.substr(0, input.find(delim));

        if (command == "upload") {
            argument = input.substr(command.size() + 1);
            std::cout << "Upload " << argument << "\n";
            send_file(argument);
        }
        else if (command == "download") {
            argument = input.substr(command.size() + 1);
            std::cout << "Download " << argument << "\n";
            get_file(argument);
        }
        else if (command == "delete") {
            argument = input.substr(command.size() + 1);
            std::cout << "Delete " << argument << "\n";
            //delete_file(argument);
        }
        else if (command == "exit") {
            close_connection();
        }
        else if (command == "list_server") {
            std::cout << "ListServer\n";
            list_server_files();
        }
        else if (command == "list_client") {
            std::cout << "ListClient\n";
            //list_local_files();
        }
        else if (command == "get_sync_dir") {
            std::cout << "GetSyncDir\n";
            //sync_client();
        }
        else {
            std::cout << "Comando não reconhecido\n";
        }
    }
    while (command != "exit");
}

void send_file(std::string absolute_filename) {
    std::lock_guard<std::mutex> lock(socket_mutex);

    ssize_t bytes;
    FILE *file;

    fs::path absolute_path(absolute_filename);

    if (fs::exists(absolute_path) && fs::is_regular_file(absolute_path)) {
        if ((file = fopen(absolute_filename.c_str(), "rb")) != nullptr) {

            // Envia o comando
            Command command = Upload;
            write_socket(socket_fd, (const void *) &command, sizeof(command));

            // Envia o nome do arquivo
            std::cout << "Enviando o nome do arquivo\n";
            std::string filename = absolute_path.filename().string();
            send_string(socket_fd, filename);
            std::cout << "Nome do arquivo enviado\n";

            // Envia o tanho do arquivo
            size_t file_size = fs::file_size(absolute_path);
            write_socket(socket_fd, (const void *) &file_size, sizeof(file_size));

            // Envia a data de modificação do arquivo
            time_t time = fs::last_write_time(absolute_path);
            write_socket(socket_fd, (const void *) &time, sizeof(time));

            // Recebe a confirmação de upload do servidor.
            if (!read_bool(socket_fd)) {
                std::cout << "Arquivo " << absolute_path.string() << " não precisa ser enviado\n";
                fclose(file);
                return;
            }

            bool file_open_ok = read_bool(socket_fd);
            if (!file_open_ok) {
                std::cerr << "O arquivo não conseguiu ser aberto no servidor\n";
                fclose(file);
                return;
            }

            std::cout << "Preparando para enviar os bytes do arquivo\n";
            // Se o servidor quiser o arquivo, envia os bytes

            // Envia o arquivo
            send_file(socket_fd, file, file_size);
            fclose(file);
        }
        std::cout << "Arquivo " << absolute_path.string() << " enviado\n";
    }
    else {
        std::cerr << "Arquivo " << absolute_filename << " não existe\n";
    }
}

void get_file(std::string filename) {
    std::lock_guard<std::mutex> lock(socket_mutex);

    Command command = Download;
    write_socket(socket_fd, (const void *) &command, sizeof(command));

    send_string(socket_fd, filename);

    bool exists = read_bool(socket_fd);
    if (!exists) {
        std::cerr << "Servidor informou que arquivo não existe\n";
        return;
    }

    size_t file_size;
    read_socket(socket_fd, (void *) &file_size, sizeof(file_size));

    std::cout << "Tamanho do arquivo a receber: " << file_size << " bytes\n";

    fs::path absolute_path = fs::current_path() / fs::path(filename);
    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cout << "Erro ao abrir o arquivo para escrita\n";
        send_bool(socket_fd, false);
        return;
    }
    send_bool(socket_fd, true);

    read_file(socket_fd, file, file_size);
    fclose(file);
}

void close_connection() {
    std::lock_guard<std::mutex> lock(socket_mutex);

    Command command = Exit;
    write_socket(socket_fd, (const void *) &command, sizeof(command));
    close(socket_fd);
}

void list_server_files() {
    std::lock_guard<std::mutex> lock(socket_mutex);

    // Envia o comando para listar os arquivos.
    Command command = ListServer;
    write_socket(socket_fd, (const void *) &command, sizeof(command));

    // Lê o tamamnho da lista
    size_t n;
    read_socket(socket_fd, (void *) &n, sizeof(n));

    std::vector<FileInfo> files;
    files.reserve(n);

    // Se prepara para receber a lista dos arquivos.
    for (int i = 0; i < n; ++i) {
        FileInfo file_info;
        read_socket(socket_fd, (void *) &file_info, sizeof(file_info));
        files.push_back(file_info);
    }

    for (auto &file : files) {
        std::cout << file.filename() << "\n";
    }
}
