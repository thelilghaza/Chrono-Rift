#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <cstdlib>

int main(int argc, char** argv) {
    std::string mode = "single";
    if (argc >= 3 && std::string(argv[1]) == "--multiplayer") {
        mode = argv[2];
    } else if (argc >= 2 && std::string(argv[1]) == "--single") {
        mode = "single";
    }

    std::cout << "[Launcher] Starting in mode: " << mode << "\n";
    setenv("GAME_MODE", mode.c_str(), 1);

    pid_t gui_pid = fork();
    if (gui_pid == 0) {
        execl("./gui/game_gui", "game_gui", nullptr);
        perror("execl gui");
        exit(1);
    }

    pid_t hip1_pid = fork();
    if (hip1_pid == 0) {
        execl("./hip/hip", "hip", "--player-id", "1", nullptr);
        perror("execl hip 1");
        exit(1);
    }

    if (mode == "pvp" || mode == "coop") {
        pid_t hip2_pid = fork();
        if (hip2_pid == 0) {
            execl("./hip/hip", "hip", "--player-id", "2", nullptr);
            perror("execl hip 2");
            exit(1);
        }
    }

    /* Spec §11: PvP "compete against each other or against a shared pool of
     * NPCs." Always spawn the Strategic process so enemy threads are present
     * in every mode; without it the Arbiter's NPC scheduling slots would
     * permanently auto-Skip. */
    {
        pid_t asp_pid = fork();
        if (asp_pid == 0) {
            execl("./asp/asp", "asp", nullptr);
            perror("execl asp");
            exit(1);
        }
    }

    // Replace the launcher process with the Arbiter.
    // The Arbiter will inherit the child processes and can receive SIGCHLD for them.
    execl("./arbiter/arbiter", "arbiter", nullptr);
    perror("execl arbiter");
    return 1;
}
