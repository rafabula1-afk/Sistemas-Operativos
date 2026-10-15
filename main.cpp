#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cctype>
#include <pwd.h>
#include <unistd.h>
#include <ncurses.h>
#include <algorithm>
#include <csignal>
#include <unordered_map>
#include <deque>
#include <sys/resource.h>

using namespace std;
namespace fs = std::filesystem;

struct CpuStats {
    long long totalTime;
    long long idleTime;
};

struct ProcessInfo {
    int pid;
    int ppid;
    string user;
    string state;      
    long priority;
    long long threads;
    long long memoryKB;
    double memPercent;
    double cpuPercent;  
    long long cpuTimeTicks;
    string cpuTimeFormatted; // mm:ss
    string cmdline;
    int depth = 0; 
};

struct ThreadInfo {
    int tid;
    string state;
    string name;
};


unordered_map<int, long long> prevProcCpuTicks;
unordered_map<int, deque<double>> procCpuHistory; 

// --- LECTURA DE SISTEMA ---
long long getTotalMemory(long long &memUsed, long long &memAvailable) {
    ifstream file("/proc/meminfo");
    string key, unit;
    long long value, memTotal = 0;
    memAvailable = 0;
    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") memTotal = value;
        else if (key == "MemAvailable:") memAvailable = value;
    }
    memUsed = memTotal - memAvailable;
    return memTotal;
}

CpuStats getCpuStats() {
    ifstream file("/proc/stat");
    string cpuLabel;
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    file >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    return {user + nice + system + idle + iowait + irq + softirq + steal, idle + iowait};
}

string getLoadAverage() {
    ifstream file("/proc/loadavg");
    string l1, l5, l15;
    if (file >> l1 >> l5 >> l15) {
        return l1 + " " + l5 + " " + l15;
    }
    return "N/A";
}

bool isNumeric(const string& str) {
    for (char c : str) if (!isdigit(c)) return false;
    return !str.empty();
}

ProcessInfo getProcessDetails(int pid, long long globalMemTotal, double systemCpuDiff) {
    ProcessInfo proc;
    proc.pid = pid; proc.memoryKB = 0; proc.threads = 1; proc.ppid = 0; 
    proc.priority = 0; proc.cpuTimeTicks = 0; proc.cpuPercent = 0.0; proc.user = "unknown";
    string path = "/proc/" + to_string(pid);

    ifstream statFile(path + "/stat");
    if (statFile.is_open()) {
        string comm;
        int pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        long minflt, cminflt, majflt, cmajflt, utime, stime, cutime, cstime, nice;
        statFile >> proc.pid >> comm >> proc.state >> proc.ppid >> pgrp >> session >> tty_nr >> tpgid 
                 >> flags >> minflt >> cminflt >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime 
                 >> proc.priority >> nice >> proc.threads;
        
        proc.cpuTimeTicks = utime + stime;

        // Formatear tiempo acumulado a mm:ss 
        long ticksPerSec = sysconf(_SC_CLK_TCK);
        long totalSecs = proc.cpuTimeTicks / (ticksPerSec > 0 ? ticksPerSec : 100);
        long mins = totalSecs / 60;
        long secs = totalSecs % 60;
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%02ld:%02ld", mins, secs);
        proc.cpuTimeFormatted = string(buffer);

        // %CPU en vivo
        if (systemCpuDiff > 0 && prevProcCpuTicks.find(pid) != prevProcCpuTicks.end()) {
            double procCpuDiff = proc.cpuTimeTicks - prevProcCpuTicks[pid];
            proc.cpuPercent = (procCpuDiff / systemCpuDiff) * 100.0;
            if (proc.cpuPercent < 0) proc.cpuPercent = 0.0;
        }
        prevProcCpuTicks[pid] = proc.cpuTimeTicks;

        // Guardar Histórico 
        procCpuHistory[pid].push_back(proc.cpuPercent);
        if (procCpuHistory[pid].size() > 5) {
            procCpuHistory[pid].pop_front();
        }
    }

    ifstream statusFile(path + "/status");
    if (statusFile.is_open()) {
        string line, key;
        while (statusFile >> key) {
            if (key == "VmRSS:") {
                long long value; statusFile >> value;
                proc.memoryKB = value;
                proc.memPercent = (static_cast<double>(proc.memoryKB) / globalMemTotal) * 100.0;
            } else if (key == "Uid:") {
                int uid; statusFile >> uid;
                struct passwd *pw = getpwuid(uid);
                if (pw) proc.user = pw->pw_name;
            }
            getline(statusFile, line);
        }
    }

    ifstream cmdFile(path + "/cmdline");
    if (cmdFile.is_open()) {
        string cmd; getline(cmdFile, cmd);
        for (char& c : cmd) if (c == '\0') c = ' ';
        if (cmd.empty()) cmd = "[" + proc.user + "_worker]";
        proc.cmdline = cmd;
    }
    return proc;
}

vector<ProcessInfo> getAllProcesses(long long globalMemTotal, double systemCpuDiff, int &runningCount, int &stoppedCount, int &zombieCount) {
    vector<ProcessInfo> processes;
    runningCount = 0; stoppedCount = 0; zombieCount = 0;

    for (const auto& entry : fs::directory_iterator("/proc")) {
        if (entry.is_directory()) {
            string folderName = entry.path().filename().string();
            if (isNumeric(folderName)) {
                ProcessInfo p = getProcessDetails(stoi(folderName), globalMemTotal, systemCpuDiff);
                
                if (p.state == "R") runningCount++;
                else if (p.state == "T") stoppedCount++;
                else if (p.state == "Z") zombieCount++;

                processes.push_back(p);
            }
        }
    }
    return processes;
}

// LECTURA DE HILOS
vector<ThreadInfo> getProcessThreads(int pid) {
    vector<ThreadInfo> threads;
    string taskPath = "/proc/" + to_string(pid) + "/task";
    if (!fs::exists(taskPath)) return threads;

    for (const auto& entry : fs::directory_iterator(taskPath)) {
        if (entry.is_directory()) {
            string tidStr = entry.path().filename().string();
            if (isNumeric(tidStr)) {
                int tid = stoi(tidStr);
                string statPath = taskPath + "/" + tidStr + "/stat";
                ifstream statFile(statPath);
                if (statFile.is_open()) {
                    int id; string comm; string state;
                    statFile >> id >> comm >> state;
                    if (!comm.empty() && comm.front() == '(' && comm.back() == ')') {
                        comm = comm.substr(1, comm.length() - 2);
                    }
                    threads.push_back({tid, state, comm});
                }
            }
        }
    }
    return threads;
}

// CONSTRUCCIÓN DEL ÁRBOl
void buildTreeRecursive(int parentPid, unordered_map<int, vector<ProcessInfo>>& childrenMap, vector<ProcessInfo>& treeList, int depth) {
    if (childrenMap.find(parentPid) == childrenMap.end()) return;
    
    for (auto& proc : childrenMap[parentPid]) {
        proc.depth = depth;
        treeList.push_back(proc);
        buildTreeRecursive(proc.pid, childrenMap, treeList, depth + 1);
    }
}

vector<ProcessInfo> buildProcessTree(const vector<ProcessInfo>& procs) {
    unordered_map<int, vector<ProcessInfo>> childrenMap;
    unordered_map<int, bool> existsMap;

    for (const auto& p : procs) {
        childrenMap[p.ppid].push_back(p);
        existsMap[p.pid] = true;
    }

    vector<ProcessInfo> treeList;
    for (const auto& p : procs) {
        if (p.ppid == 0 || existsMap.find(p.ppid) == existsMap.end()) {
            ProcessInfo root = p;
            root.depth = 0;
            treeList.push_back(root);
            buildTreeRecursive(root.pid, childrenMap, treeList, 1);
        }
    }
    return treeList;
}

// --- MAIN ---
int main() {
    initscr();            
    noecho();             
    cbreak();             
    curs_set(0);          
    keypad(stdscr, TRUE); 

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_RED, COLOR_BLACK);     
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);  // Espera I/O (D)
        init_pair(3, COLOR_CYAN, COLOR_BLACK);    // Encabezado
        init_pair(4, COLOR_GREEN, COLOR_BLACK);   // Información
    }

    bool running = true;
    int selectedRow = 0;
    int scrollOffset = 0;
    char sortMode = 'c'; 
    string searchQuery = "";
    bool isSearching = false;
    bool isTreeView = false;   
    bool isThreadView = false;

    int refreshMs = 1000;
    timeout(refreshMs);

    string actionMessage = "";
    bool pendingKillConfirm = false;
    int targetPidToKill = -1;
    bool isSigkillAction = false;

    CpuStats prevCpu = getCpuStats();

    while (running) {
        clear(); 

        CpuStats currCpu = getCpuStats();
        double totalDiff = currCpu.totalTime - prevCpu.totalTime;
        double idleDiff = currCpu.idleTime - prevCpu.idleTime;
        double cpuUsagePercent = 0.0;
        if (totalDiff > 0) {
            cpuUsagePercent = ((totalDiff - idleDiff) / totalDiff) * 100.0;
        }

        long long memUsed = 0, memAvailable = 0;
        long long memTotal = getTotalMemory(memUsed, memAvailable);
        string loadAvg = getLoadAverage();

        int runningCount = 0, stoppedCount = 0, zombieCount = 0;
        vector<ProcessInfo> allProcs = getAllProcesses(memTotal, totalDiff, runningCount, stoppedCount, zombieCount);
        prevCpu = currCpu;

        vector<ProcessInfo> procs;
        if (!searchQuery.empty()) {
            string queryLower = searchQuery;
            transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);
            for (const auto& p : allProcs) {
                string cmdLower = p.cmdline;
                transform(cmdLower.begin(), cmdLower.end(), cmdLower.begin(), ::tolower);
                if (cmdLower.find(queryLower) != string::npos) {
                    procs.push_back(p);
                }
            }
        } else {
            procs = allProcs;
        }

        if (isTreeView) {
            procs = buildProcessTree(procs);
        } else {
            if (sortMode == 'm') {
                sort(procs.begin(), procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.memPercent > b.memPercent; });
            } else if (sortMode == 'c') {
                sort(procs.begin(), procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.cpuPercent > b.cpuPercent; });
            } else if (sortMode == 'p') {
                sort(procs.begin(), procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
            }
        }

        if (procs.empty()) {
            procs.push_back({0, 0, "N/A", "N", 0, 0, 0, 0.0, 0.0, 0, "00:00", "Sin coincidencias"});
        }
        if (selectedRow >= procs.size()) selectedRow = procs.size() - 1;

        // VISTA DE HILOS
        if (isThreadView) {
            int currentPid = procs[selectedRow].pid;
            vector<ThreadInfo> threads = getProcessThreads(currentPid);

            string shortCmd = procs[selectedRow].cmdline;
            if (shortCmd.length() > 50) shortCmd = shortCmd.substr(0, 50) + "...";

            if (has_colors()) attron(COLOR_PAIR(3) | A_BOLD);
            mvprintw(0, 0, "=== VISTA DE HILOS | PID: %d ===", currentPid);
            if (has_colors()) attroff(COLOR_PAIR(3) | A_BOLD);

            mvprintw(1, 0, "Comando: %s", shortCmd.c_str());
            mvprintw(2, 0, "Total de hilos: %lu | Presione 'h' o ESC para volver", threads.size());
            
            attron(A_REVERSE);
            mvprintw(4, 0, "%-12s %-8s %s", "TID (Hilo)", "ESTADO", "NOMBRE DEL HILO");
            attroff(A_REVERSE);

            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);

            for (size_t t = 0; t < threads.size() && (int)t < max_y - 6; ++t) {
                mvprintw(5 + t, 0, "%-12d %-8s %s", threads[t].tid, threads[t].state.c_str(), threads[t].name.c_str());
            }

            refresh();
            int ch = getch();
            if (ch == 'h' || ch == 'H' || ch == 27) {
                isThreadView = false;
            }
            continue;
        }

        // RENDERIZADO PRINCIPAL
        if (has_colors()) attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(0, 0, "=== MONITOR DE SISTEMA INTERACTIVO ===");
        if (has_colors()) attroff(COLOR_PAIR(3) | A_BOLD);

        mvprintw(1, 0, "CPU Global: %5.1f%% | RAM: %lld MB / %lld MB (Libre: %lld MB) | LoadAvg: %s", 
                 cpuUsagePercent, memUsed / 1024, memTotal / 1024, memAvailable / 1024, loadAvg.c_str());
        mvprintw(2, 0, "Procesos Totales: %lu | Activos (R): %d | Detenidos (T): %d | Zombies (Z): %d", 
                 allProcs.size(), runningCount, stoppedCount, zombieCount);
        
        mvprintw(3, 0, "Controles: m (Mem) | c (%%CPU) | p (PID) | t (Arbol) | h (Hilos) | / (Buscar) | k (Terminar) | K (SIGKILL) | s/r (Susp/Rean) | +/- (Prio) | i (Refresh) | q (Salir)");

        // Muestra de Histórico de CPU en barra informativa (Punto 7)
        string historyStr = "";
        if (procCpuHistory.find(procs[selectedRow].pid) != procCpuHistory.end()) {
            for (double val : procCpuHistory[procs[selectedRow].pid]) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.1f%% ", val);
                historyStr += buf;
            }
        }

        if (pendingKillConfirm) {
            attron(A_REVERSE | COLOR_PAIR(1));
            mvprintw(4, 0, "CONFIRMACION: ¿%s proceso PID %d? Presione 's' (Si) o 'n' (No)", 
                     isSigkillAction ? "ELIMINAR POR LA FUERZA (SIGKILL)" : "TERMINAR (SIGTERM)", targetPidToKill);
            attroff(A_REVERSE | COLOR_PAIR(1));
        } else if (isSearching) {
            mvprintw(4, 0, "BUSCAR: %s_ (ENTER para confirmar, ESC para borrar)", searchQuery.c_str());
        } else {
            mvprintw(4, 0, "Filtro: '%s' | Hist. CPU PID %d: [%s] %s", 
                     searchQuery.c_str(), procs[selectedRow].pid, historyStr.c_str(), actionMessage.c_str());
        }

        // CABECERA COMPLETA DE 11 COLUMNAS
        attron(A_REVERSE); 
        mvprintw(6, 0, "%-6s %-6s %-9s %-3s %-4s %-5s %-5s %-6s %-5s %-6s %s", 
                 "PID", "PPID", "USUARIO", "EST", "PRIO", "HILOS", "%CPU", "RES(MB)", "%MEM", "TIME", "COMANDO");
        attroff(A_REVERSE); 

        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x); 
        int max_printable_rows = max_y - 8; 

        if (selectedRow < scrollOffset) {
            scrollOffset = selectedRow; 
        } else if (selectedRow >= scrollOffset + max_printable_rows) {
            scrollOffset = selectedRow - max_printable_rows + 1; 
        }
        
        int row = 7;
        for (int i = scrollOffset; i < procs.size() && row < max_y - 1; ++i) {
            string cmd = procs[i].cmdline;
            if (isTreeView) {
                string indent = "";
                for (int d = 0; d < procs[i].depth && d < 4; ++d) indent += "  ";
                cmd = indent + "|-- " + cmd;
            }
            if (cmd.length() > 25) cmd = cmd.substr(0, 25); 

            bool isZombie = (procs[i].state == "Z");
            bool isWaitingIO = (procs[i].state == "D");

            if (i == selectedRow) attron(A_REVERSE);
            else if (isZombie && has_colors()) attron(COLOR_PAIR(1) | A_BOLD);
            else if (isWaitingIO && has_colors()) attron(COLOR_PAIR(2) | A_BOLD);

            mvprintw(row, 0, "%-6d %-6d %-9s %-3s %-4ld %-5lld %-5.1f %-6lld %-5.1f %-6s %s",
                     procs[i].pid, procs[i].ppid, procs[i].user.substr(0,8).c_str(), 
                     procs[i].state.c_str(), procs[i].priority, procs[i].threads, 
                     procs[i].cpuPercent, procs[i].memoryKB / 1024, procs[i].memPercent, 
                     procs[i].cpuTimeFormatted.c_str(), cmd.c_str());
                     
            if (i == selectedRow) attroff(A_REVERSE);
            else if (isZombie && has_colors()) attroff(COLOR_PAIR(1) | A_BOLD);
            else if (isWaitingIO && has_colors()) attroff(COLOR_PAIR(2) | A_BOLD);
            
            row++;
        }

        refresh(); 

        int ch = getch(); 

        if (pendingKillConfirm) {
            if (ch == 's' || ch == 'S') {
                int sig = isSigkillAction ? SIGKILL : SIGTERM;
                if (kill(targetPidToKill, sig) == 0) {
                    actionMessage = "| Accion sobre PID " + to_string(targetPidToKill) + " aplicada con exito.";
                } else {
                    actionMessage = "| Error al aplicar accion sobre PID " + to_string(targetPidToKill);
                }
                pendingKillConfirm = false;
            } else if (ch == 'n' || ch == 'N' || ch == 27) {
                actionMessage = "| Accion cancelada.";
                pendingKillConfirm = false;
            }
        } else if (isSearching) {
            if (ch == 10 || ch == KEY_ENTER) {
                isSearching = false;
            } else if (ch == 27) {
                searchQuery = "";
                isSearching = false;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (!searchQuery.empty()) searchQuery.pop_back();
            } else if (ch >= 32 && ch <= 126) {
                searchQuery += static_cast<char>(ch);
                selectedRow = 0;
            }
        } else {
            switch(ch) {
                case 'q': case 'Q': running = false; break;
                case KEY_UP: if (selectedRow > 0) selectedRow--; break;
                case KEY_DOWN: if (selectedRow < procs.size() - 1) selectedRow++; break;
                case 'm': case 'M': sortMode = 'm'; isTreeView = false; selectedRow = 0; scrollOffset = 0; break;
                case 'c': case 'C': sortMode = 'c'; isTreeView = false; selectedRow = 0; scrollOffset = 0; break;
                case 'p': case 'P': sortMode = 'p'; isTreeView = false; selectedRow = 0; scrollOffset = 0; break;
                case 't': case 'T': isTreeView = !isTreeView; selectedRow = 0; scrollOffset = 0; break; 
                case 'h': case 'H': isThreadView = true; break; 
                case '/': 
                    isSearching = true; 
                    curs_set(1); 
                    break;
                case 'k': // Terminar suave (SIGTERM)
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        pendingKillConfirm = true;
                        isSigkillAction = false;
                        targetPidToKill = procs[selectedRow].pid;
                    }
                    break;
                case 'K': // Terminar forzoso (SIGKILL)
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        pendingKillConfirm = true;
                        isSigkillAction = true;
                        targetPidToKill = procs[selectedRow].pid;
                    }
                    break;
                case 's': case 'S': 
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        if (kill(procs[selectedRow].pid, SIGSTOP) == 0) {
                            actionMessage = "| Accion: PID " + to_string(procs[selectedRow].pid) + " suspendido.";
                        } else {
                            actionMessage = "| Error al suspender PID " + to_string(procs[selectedRow].pid);
                        }
                    }
                    break;
                case 'r': case 'R': 
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        if (kill(procs[selectedRow].pid, SIGCONT) == 0) {
                            actionMessage = "| Accion: PID " + to_string(procs[selectedRow].pid) + " reanudado.";
                        } else {
                            actionMessage = "| Error al reanudar PID " + to_string(procs[selectedRow].pid);
                        }
                    }
                    break;
                case '+': 
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        int currentNice = getpriority(PRIO_PROCESS, procs[selectedRow].pid);
                        if (setpriority(PRIO_PROCESS, procs[selectedRow].pid, currentNice + 1) == 0) {
                            actionMessage = "| Prioridad de PID " + to_string(procs[selectedRow].pid) + " cambiada a " + to_string(currentNice + 1);
                        } else {
                            actionMessage = "| Error: Sin permisos para cambiar prioridad.";
                        }
                    }
                    break;
                case '-': 
                    if (!procs.empty() && procs[selectedRow].pid > 0) {
                        int currentNice = getpriority(PRIO_PROCESS, procs[selectedRow].pid);
                        if (setpriority(PRIO_PROCESS, procs[selectedRow].pid, currentNice - 1) == 0) {
                            actionMessage = "| Prioridad de PID " + to_string(procs[selectedRow].pid) + " cambiada a " + to_string(currentNice - 1);
                        } else {
                            actionMessage = "| Error: Requiere 'sudo' para aumentar prioridad.";
                        }
                    }
                    break;
                case 'i': case 'I': 
                    if (refreshMs == 1000) refreshMs = 500;
                    else if (refreshMs == 500) refreshMs = 2000;
                    else refreshMs = 1000;
                    timeout(refreshMs);
                    actionMessage = "| Refresh Rate modificado a " + to_string(refreshMs) + "ms";
                    break;
            }
            if (!isSearching) curs_set(0);
        }
    }

    endwin();
    return 0;
}