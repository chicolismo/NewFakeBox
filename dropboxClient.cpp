#include <netinet/in.h>
#include "dropboxClient.h"
#include "dropboxUtil.h"
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <strings.h>
#include "dropboxServer.h"
#include <boost/filesystem.hpp>
#include "Inotify-master/FileSystemEvent.h"
#include "Inotify-master/Inotify.h"
#include <sstream>
#include <thread>
#include <netdb.h>
#include <set>

namespace fs = boost::filesystem;

std::mutex socket_mutex;
std::mutex io_mutex;
std::mutex sync_mutex;

// O id do usuário
std::string user_id;

fs::path user_dir;

uint16_t port_number;

// O endereço do servidor
sockaddr_in server_address{};

// Socket para se comunicar com o servidor.
int socket_fd;

// Objeto que vai ficar escutando mudancas no diretorio do cliente.
Inotify inotify(IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE | IN_CLOSE_WRITE);

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

    create_sync_dir();
    sync_client();
    
    // Manda a global inotify cuidar do diretório de sincronização
    inotify.watchDirectoryRecursively(user_dir);

    // Criação de thread de sincronização
    std::thread thread;
    thread = std::thread(run_sync_thread);
    if (!thread.joinable()) {
        std::cerr << "Erro ao criar thread de sincronização\n";
        close_connection();
        return 1;
    }
    thread.detach();

    // Exibe a interface
    run_interface();
}

void run_sync_thread() {
    while (true) {
        FileSystemEvent event = inotify.getNextEvent();

        if (event.mask & IN_DELETE || event.mask & IN_MOVED_FROM) {
            send_delete_command(event.path.filename().string());
        }
        //else if (event.mask & IN_CLOSE_WRITE || event.mask & IN_CREATE || event.mask & IN_MOVED_TO) {
        else if (event.mask & IN_CLOSE_WRITE || event.mask & IN_CREATE) {
            send_file(event.path.string());
        }
    }
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
            delete_file(argument);
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
            list_local_files();
        }
        else if (command == "get_sync_dir") {
            std::cout << "GetSyncDir\n";
            sync_client();
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
    get_file(filename, true);
}

void get_file(std::string filename, bool current_path) {
    std::lock_guard<std::mutex> lock(socket_mutex);
    //std::lock_guard<std::mutex> lock(io_mutex);

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

    fs::path absolute_path;
    if (current_path) {
        absolute_path = fs::current_path() / fs::path(filename);
    } else {
        absolute_path = user_dir / fs::path(filename);
    }
    
    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cout << "Erro ao abrir o arquivo para escrita\n";
        send_bool(socket_fd, false);
        return;
    }
    send_bool(socket_fd, true);

    read_file(socket_fd, file, file_size);
    fclose(file);
    
    std::cout << "Preparando-se para mudar a data do arquivo " << absolute_path.string() << "\n";
    
    time_t time;
    read_socket(socket_fd, (void *) &time, sizeof(time));

    std::cout << "Data recebida\n";
    fs::last_write_time(absolute_path, time);
   
}

void close_connection() {
    std::lock_guard<std::mutex> lock(socket_mutex);

    Command command = Exit;
    write_socket(socket_fd, (const void *) &command, sizeof(command));
    close(socket_fd);
}

void list_server_files() {
    std::vector<FileInfo> server_files = get_server_files();

    std::cout << "=====================\n";
    std::cout << "Arquivos no servidor:\n";
    std::cout << "=====================\n\n";
    for (auto &file : server_files) {
        std::cout << file.string() << "\n";
    }
}

void list_local_files() {
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(user_dir);
    
    char date_buffer[20];
    
    std::cout << "====================\n";
    std::cout << "Arquivos no cliente:\n";
    std::cout << "====================\n\n";
    while (dir_iter != end_iter) {
        if (fs::is_regular_file(dir_iter->path())) {
            
            std::cout << "Nome: " << dir_iter->path().filename() << "\n";
            std::cout << "Tamanho: " << fs::file_size(dir_iter->path()) << " bytes\n";
            
            time_t date = fs::last_write_time(dir_iter->path());
            strftime(date_buffer, 20, "%Y-%m-%d %H:%M:%S", localtime(&date));
            
            std::cout << "Modificado: " << date_buffer << "\n\n";
        }
        ++dir_iter;
    }
}

void create_sync_dir() {
    fs::path home_dir(getenv("HOME"));
    fs::path sync_dir("sync_dir_" + user_id);
    fs::path fullpath = home_dir / sync_dir;
    
    // Global boost::filesystem::path
    user_dir = fullpath;
    
    if (!fs::exists(fullpath)) {
        fs::create_directory(fullpath);
    }
}

std::vector<FileInfo> get_server_files() {
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

    return std::move(files);
}

void delete_file(std::string filename) {
    std::lock_guard<std::mutex> lock(io_mutex);

    fs::path filepath = user_dir / fs::path(filename);

    bool deleted = fs::remove(filepath);

    if (deleted) {
        std::cout << "Arquivo " << filename << " removido\n";
        //send_delete_command(filename);
    }
    else {
        std::cout << "Arquivo " << filename << " não existe no diretório de sincronização\n";
    }
}

void send_delete_command(std::string filename) {
    std::lock_guard<std::mutex> lock(socket_mutex);
    
    Command command = Delete;

    // Envia o comando de Delete para o servidor
    ssize_t bytes;
    bytes = write(socket_fd, (void *) &command, sizeof(command));
    if (bytes < 0) {
        std::cerr << "Erro ao enviar o comando Delete para o servidor\n";
    }
    else {
        // Envia o nome do arquivo para o servidor
        write(socket_fd, (void *)(filename.c_str()), BUFFER_SIZE);
    }
}

void sync_client() {
    std::lock_guard<std::mutex> io_lock(io_mutex);
    std::lock_guard<std::mutex> sync_lock(sync_mutex);
    
    std::vector<FileInfo> server_files = get_server_files();
    
    // Arquivos
    std::set<std::string> files_on_server;
    
    std::set<std::string> files_to_send_to_server;
    
    for (FileInfo &file_info : server_files) {
        fs::path absolute_path = user_dir / fs::path(file_info.filename());
        
        files_on_server.insert(file_info.filename());
               
        bool exists = fs::exists(absolute_path);
        if ((exists && (fs::last_write_time(absolute_path) < file_info.last_modified())) || !exists) {
            get_file(file_info.filename(), false);
            
        }
        else {
            if (fs::last_write_time(absolute_path) > file_info.last_modified()) {
                fs::path filename = user_dir / fs::path(file_info.filename());
                files_to_send_to_server.insert(filename.string());
            }
        }
    }
    
    // Determina quais arquivos enviar para o servidor.   
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(user_dir);
    while (dir_iter != end_iter) {
        if (fs::is_regular_file(dir_iter->path())) {
            std::string filename = dir_iter->path().filename().string();
               
            // Se o arquivo no diretório do cliente não existe nos arquivos
            // enviados pelo servidor, devemos inserir seu nome para enviar. 
            auto it = files_on_server.find(filename);
            if (it == files_on_server.end()) {
                files_to_send_to_server.insert(dir_iter->path().string());
            }
        }
        ++dir_iter;
    }
    
    /*
    std::cout << "Arquivos no servidor\n";
    for (auto &name : files_on_server) {
        std::cout << name << "\n";
    }
    */
    
    std::cout << "\n\nArquivo para enviar para o servidor\n";
    for (auto &filename : files_to_send_to_server) {
        std::cout << "Enviando " << filename << " para o servidor\n";
        send_file(filename);
    }
    
    
}
