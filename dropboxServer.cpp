#include <sys/types.h>
#include <sys/socket.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <memory.h>
#include <thread>
#include "dropboxServer.h"
#include "dropboxUtil.h"
#include "dropboxClient.h"
#include <boost/filesystem.hpp>


namespace fs = boost::filesystem;

// Globais
fs::path server_dir;

uint16_t port_number;
sockaddr_in address{};
ClientDict clients;
std::mutex connection_mutex;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Informe a porta\n";
        std::exit(1);
    }

    char *end;
    port_number = static_cast<uint16_t >(std::strtol(argv[1], &end, 10));

    bzero((void *) &address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port_number);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    // Criando o socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Binding
    if (bind(socket_fd, (sockaddr *) &address, sizeof(address)) < 0) {
        std::cerr << "Erro ao fazer o binding\n";
        close(socket_fd);
        std::exit(1);
    }

    // Listening
    listen(socket_fd, 5);

    // Determina o diretório atual
    server_dir = fs::current_path();

    // Inicializa os clientes
    initialize_clients();

    std::cout << "O servidor está aguarando conexões na porta " << port_number << "\n";

    // Aguardando conexões
    while (true) {
        int new_socket_fd;
        {
            sockaddr_in client_address{};
            socklen_t client_len = sizeof(client_address);
            new_socket_fd = accept(socket_fd, (sockaddr *) &client_address, &client_len);
        }

        if (new_socket_fd == -1) {
            std::cerr << "Erro ao aceitar o socket do cliente\n";
            continue;
        }

        ConnectionType type;
        bzero(&type, sizeof(type));
        ssize_t bytes = read_socket(new_socket_fd, (void *) &type, sizeof(type));
        if (bytes == -1) {
            std::cerr << "Erro ao ler o tipo de conexão do cliente\n";
            close(socket_fd);
            std::exit(1);
        }

        // Se o cliente está se conectando normalmente
        std::thread thread;
        if (type == Normal) {
            std::cout << "Running normal thread\n";
            thread = std::thread(run_normal_thread, new_socket_fd);
            thread.detach();
        }
        else {
            close(new_socket_fd);
        }

    }

    close(socket_fd);

    /*
    */
}

void run_normal_thread(int client_socket_fd) {
    // Temos que ler o user_id
    std::string user_id = receive_string(client_socket_fd);

    std::cout << user_id << " está tentando se conectar\n";

    // Tenta conectar
    bool is_connected = false;
    is_connected = connect_client(user_id, client_socket_fd);
    write_socket(client_socket_fd, (const void *) &is_connected, sizeof(is_connected));

    // Se a conexão for bem sucedida, rodar função que espera pelos comandos
    if (is_connected) {
        std::cout << user_id << " se conectou ao servidor\n";

        run_user_interface(user_id, client_socket_fd);
    }

    // Se a conexão for mal sucedida, retorna;
    return;
}

void initialize_clients() {
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(server_dir);

    while (dir_iter != end_iter) {
        if (fs::is_directory(dir_iter->path())) {
            std::string user_id(fs::basename(dir_iter->path().string()));

            clients[user_id] = new Client(user_id);

            fs::directory_iterator client_dir_iter(dir_iter->path());

            while (client_dir_iter != end_iter) {
                if (fs::is_regular_file(client_dir_iter->path())) {
                    FileInfo file_info;

                    fs::path filepath(client_dir_iter->path());

                    file_info.set_filename(filepath.filename().string());

                    file_info.set_extension(fs::extension(filepath));

                    file_info.set_last_modified(fs::last_write_time(filepath));

                    file_info.set_bytes(fs::file_size(filepath));

                    clients[user_id]->files.push_back(file_info);
                }
                ++client_dir_iter;
            }
        }
        ++dir_iter;
    }
}

bool connect_client(std::string user_id, int client_socket_fd) {
    // Trava a função para apenas uma thread de cada vez.
    std::lock_guard<std::mutex> lock(connection_mutex);

    bool ok = false;

    create_user_dir(user_id);
    auto it = clients.find(user_id);

    if (it == clients.end()) {
        clients[user_id] = new Client(user_id);
        clients[user_id]->is_logged = true;
        ok = true;
    }
    else {
        for (int &device : it->second->connected_devices) {
            if (device == EMPTY_DEVICE) {
                it->second->is_logged = true;
                device = client_socket_fd;
                ok = true;
                break;
            }
        }
    }

    std::cout << "Imprimindo arquivos do cliente\n";
    it = clients.find(user_id);
    for (auto &file : it->second->files) {
        std::cout << file.filename() << "\n";
    }

    return ok;
}

void disconnect_client(std::string user_id, int client_socket_fd) {
    auto it = clients.find(user_id);

    if (it == clients.end()) {
        std::cerr << "Usuário " << user_id << " não encontrado para desconectar\n";
        return;
    }
    else {
        std::cout << "Desconectando " << user_id << "\n";

        if (it->second->connected_devices[0] == EMPTY_DEVICE) {
            it->second->connected_devices[1] = EMPTY_DEVICE;
            it->second->is_logged = false;
        }
        else if (it->second->connected_devices[1] == EMPTY_DEVICE) {
            it->second->connected_devices[0] = EMPTY_DEVICE;
            it->second->is_logged = false;
        }
        else if (it->second->connected_devices[0] == client_socket_fd) {
            it->second->connected_devices[0] = EMPTY_DEVICE;
        }
        else {
            it->second->connected_devices[1] = EMPTY_DEVICE;
        }
        //close(client_socket_fd);
    }

}

void create_user_dir(std::string user_id) {
    // Novo usuário
    fs::path user_dir = server_dir / fs::path(user_id);
    if (!fs::exists(user_dir)) {
        fs::create_directory(user_dir);
    }
}

void run_user_interface(const std::string user_id, int client_socket_fd) {
    Command command = Exit;

    do {
        read_socket(client_socket_fd, (void *) &command, sizeof(command));

        std::string filename{};

        switch (command) {
        case Upload:
            std::cout << "Upload Requested\n";
            filename = receive_string(client_socket_fd);

            std::cout << "Arquivo a ser rebido: " << filename << "\n";
            receive_file(user_id, filename, client_socket_fd);
            break;

        case Download:
            std::cout << "Download Requested\n";
            filename = receive_string(client_socket_fd);
            send_file(user_id, filename, client_socket_fd);
            break;

        case Delete:
            std::cout << "Delete Requested\n";
            break;

        case ListServer:
            std::cout << "ListServer Requested\n";
            send_file_infos(user_id, client_socket_fd);
            break;

        case GetSyncDir:
            std::cout << "GetSyncDir Requested\n";
            break;

        case Exit:
            std::cout << "Exit Requested\n";
            disconnect_client(user_id, client_socket_fd);
            break;

        default:
            std::cout << "Comando não reconhecido\n";
            break;
        }

    }
    while (command != Exit);
}

// receive_file {{{
void receive_file(std::string user_id, std::string filename, int client_socket_fd) {

    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    std::cout << "O caminho absoluto até o arquivo no servidor é " << absolute_path.string() << "\n";

    // Vamos ler o tamanho do arquivo!
    size_t file_size;
    read_socket(client_socket_fd, (void *) &file_size, sizeof(file_size));

    std::cout << "Tamanho do arquivo recebido: " << file_size << " bytes\n";

    // Recebendo a data de modificação
    time_t time;
    read_socket(client_socket_fd, (void *) &time, sizeof(time));

    // Temos que ver se o arquivo existe e se é mais antigo e se devemos recebê-lo.
    bool should_download = !(fs::exists(absolute_path) && (fs::last_write_time(absolute_path) >= time));
    send_bool(client_socket_fd, should_download);

    if (!should_download) {
        return;
    }

    // Vamos tentar abrir o arquivo
    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cerr << "Arquivo " << absolute_path << " não pode ser aberto\n";
        send_bool(client_socket_fd, false);
        return;
    }

    send_bool(client_socket_fd, true);
    // Vamos receber os bytes do arquivo.
    std::cout << "Preparando para receber os bytes do arquivo\n";

    // TODO: Receber o arquivo
    read_file(client_socket_fd, file, file_size);
    fclose(file);

    std::cout << "Arquivo " << absolute_path.string() << " recebido\n";

    // escreve a data de modificação do arquivo
    fs::last_write_time(absolute_path, time);

    // Atualiza lista de arquivos do usuário
    update_files(user_id, filename, file_size, time);

}
// }}}

// send_file {{{
void send_file(std::string user_id, std::string filename, int client_socket_fd) {
    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    std::cout << "Localizando " << absolute_path.string() << " no servidor\n";

    bool file_ok;
    FILE *file;
    if ((file_ok = fs::exists(absolute_path)) == true) {
        file = fopen(absolute_path.c_str(), "rb");

        if (file == nullptr) {
            file_ok = false;
        }
    }

    send_bool(client_socket_fd, file_ok);

    if (file_ok) {
        std::cout << "Arquivo ok\n";
    }
    else {
        std::cout << "Arquivo não ok\n";
        return;
    }

    size_t file_size = fs::file_size(absolute_path);
    write_socket(client_socket_fd, (const void *) &file_size, sizeof(file_size));

    bool ok = read_bool(client_socket_fd);
    if (ok) {
        send_file(client_socket_fd, file, file_size);
    }
    fclose(file);
}
// }}}

void update_files(std::string user_id, std::string filename, size_t file_size, time_t timestamp) {
    auto it = clients.find(user_id);

    if (it == clients.end()) {
        std::cerr << "Erro ao atualizar os arquivos do cliente, cliente " << user_id << " não encontrado.\n";
        return;
    }

    Client *client = it->second;

    FileInfo *file = nullptr;
    for (FileInfo &file_info : client->files) {
        if (file_info.filename() == filename) {
            file = &file_info;
            break;
        }
    }

    if (file != nullptr) {
        file->set_bytes(file_size);
        file->set_last_modified(timestamp);
    }
    else {
        // Arquivo não existe;
        FileInfo new_file{};
        new_file.set_filename(filename);
        new_file.set_extension(fs::path(filename).extension().string());
        new_file.set_bytes(file_size);
        new_file.set_last_modified(timestamp);
        client->files.push_back(new_file);
    }
}

void send_file_infos(std::string user_id, int client_socket_fd) {
    auto it = clients.find(user_id);

    // Testar se o cliente foi encontrado
    if (it == clients.end()) {
        std::cerr << "Erro ao enviar a lista de file_infos, client " << user_id
            << " não encontrado\n";
        return;
    }

    Client *client = it->second;
    size_t n = client->files.size();

    // Envia o tamanho da lista
    write_socket(client_socket_fd, (const void *) &n, sizeof(n));
    for (int i = 0; i < n; ++i) {
        write_socket(client_socket_fd, (const void *) &client->files[i], sizeof(client->files[i]));
    }

}
