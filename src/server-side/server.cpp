#include "server.h"
using namespace std;

mutex buckets_mtx;
mutex fds_mtx;
sem_t s;
atomic<int> req_counter(0);
ofstream log_flow;

//print log
void log(string str){
    log_flow.open(MYLOG, ofstream::out | ofstream::app);
    log_flow << str;
    log_flow.close();
}

//delay function provided by the hw requirement
void delayTime(int d) {
    struct timeval start, check, end;
    double elapsed_seconds;
    gettimeofday(&start, NULL);
    do {
        gettimeofday(&check, NULL); 
        elapsed_seconds = (check.tv_sec + (check.tv_usec/1000000.0)) - (start.tv_sec + (start.tv_usec/1000000.0)); 
    } while (elapsed_seconds < d);
}

//parse the request, update the bucket and send back the new value
void requestHelper (int client_fd, string req, vector<int> * buckets) {
    //parse request
    int delimiter = req.find(',');
    int delay = stoi(req.substr(0, delimiter));
    int bucketNum = stoi(req.substr(delimiter + 1));
    //delay
    delayTime(delay);
    //lock and add
    unique_lock<mutex> lck (buckets_mtx);
    buckets->at(bucketNum) = buckets->at(bucketNum) + delay;
    //sendBack
    stringstream ss;
    ss << buckets->at(bucketNum);
    lck.unlock();
    const char * value = ss.str().c_str();
    send(client_fd, value, strlen(value), 0);
    //closeFd
    req_counter.fetch_add(1, std::memory_order_relaxed);
    close(client_fd);
}

//Function used for create-per-request policy
void perRequestHandler(int client_fd, string req, vector<int> * buckets) {
    requestHelper(client_fd, req, buckets);
}

//Function used for pre-create policy
void preRequestHandler(list<int> * request_fds, vector<int> * buckets) {
    //Get a new fd from the shared data structure, then receive and handle the request
    while (true) {
        sem_wait(&s);
        unique_lock<mutex> lck1 (fds_mtx);
        int cur_fd = request_fds->front();
        if (cur_fd == 0) { return; }
        request_fds->pop_front();
        lck1.unlock();
        char buffer[BUFFER_SIZE];
        recv(cur_fd, buffer, BUFFER_SIZE, 0);
        string req(buffer);
        requestHelper(cur_fd, req, buckets);
    }
}

//Count time, print log every 30 seconds
void countTime() {
    clock_t start, end;
    int prev_req_counter;
    while (true) {
        if (req_counter != 0) {
            start = clock();
            break;
        }
    }
    while (true) {
        this_thread::sleep_for(chrono::milliseconds(30000));
        stringstream ss;
        ss << "Throughput(req/30 s) :" << req_counter - prev_req_counter  << endl;
        prev_req_counter = req_counter;
        string str = ss.str();
        cout << ss.str();
        log(str);
    } 
}

//Constructor of Server class, create ServerSocket and then listen on incoming requests
Server::Server(int bucketNum) {
    buckets = new vector<int>(bucketNum, 0);
    socklen_t socket_addr_len = sizeof(socket_addr);
    memset(&host_info, 0, sizeof(host_info));
    host_info.ai_family   = AF_UNSPEC;
    host_info.ai_socktype = SOCK_STREAM;
    host_info.ai_flags    = AI_PASSIVE;
    status = getaddrinfo(HOSTNAME, PORT, &host_info, &host_info_list);
    if (status != 0) {
        cerr << "Error: cannot get address info for host" << endl;
        cerr << "  (" << HOSTNAME << "," << PORT << ")" << endl;
        exit(EXIT_FAILURE);
    } //if

    socket_fd = socket(host_info_list->ai_family, 
                host_info_list->ai_socktype, 
                host_info_list->ai_protocol);
    if (socket_fd == -1) {
        cerr << "Error: cannot create socket" << endl;
        cerr << "  (" << HOSTNAME << "," << PORT << ")" << endl;
        exit(EXIT_FAILURE);
    } //if

    int yes = 1;
    status = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    status = bind(socket_fd, host_info_list->ai_addr, host_info_list->ai_addrlen);
    if (status == -1) {
        cerr << "Error: cannot bind socket" << endl;
        cerr << "  (" << HOSTNAME << "," << PORT << ")" << endl;
        exit(EXIT_FAILURE);
    } //if

    status = listen(socket_fd, 100);
    if (status == -1) {
        cerr << "Error: cannot listen on socket" << endl; 
        cerr << "  (" << HOSTNAME << "," << PORT << ")" << endl;
        exit(EXIT_FAILURE);
    } //if
    thread t(countTime);
    t.detach();
}

//Create-per-request: keep accepting and create new threads
void Server::per_run() {
    while(true) {
        //accept
        int client_connection_fd;
        client_connection_fd = accept(socket_fd, (struct sockaddr *)&socket_addr, &socket_addr_len);
        if (client_connection_fd == -1) {
            cerr << "Error: cannot accept connection on socket" << endl;
            continue;
        } //if
        //receive
        char buffer[BUFFER_SIZE];
        recv(client_connection_fd, buffer, BUFFER_SIZE, 0);
        //create thread
        thread t(perRequestHandler, client_connection_fd, string(buffer), buckets);
        t.detach();
    }
}

//Pre-create policy
void Server::pre_run() {
    sem_init(&s, 0, 0);
    //Create shared data structure
    list<int> *request_fds = new list<int>();
    //Create thread pool
    for (int i = 0; i < POOL_SIZE; ++i) {
        thread t(preRequestHandler, request_fds, buckets);
        t.detach();
    }
    //Keep accepting and add the fd to shared data structure
    while(true) {
        int client_connection_fd;
        client_connection_fd = accept(socket_fd, (struct sockaddr *)&socket_addr, &socket_addr_len);
        if (client_connection_fd == -1) {
            cerr << "Error: cannot accept connection on socket" << endl;
            continue;
        } //if
        unique_lock<mutex> lck (fds_mtx);
        request_fds->push_back(client_connection_fd);
        sem_post(&s);
        lck.unlock();
    }
}

Server::~Server() {
    delete buckets;
    freeaddrinfo(host_info_list);
    close(socket_fd);
}

