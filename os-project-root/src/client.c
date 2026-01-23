#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char *argv[]) {
    // 1. IP Address Handling (Member 3 Requirement)
    if (argc < 2) {
        printf("Usage: %s <Server_IP>\n", argv[0]);
        return 1;
    }

    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[1024] = {0};

    // 2. Create Socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT); // Uses PORT from common.h

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // 3. Connect to Server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    printf("Connected to Game Server at %s!\n", argv[1]);

    // 4. The Main Client Loop (Listens for Server Messages)
    while (1) {
        memset(buffer, 0, 1024); // Clear buffer
        int valread = recv(sock, buffer, 1024, 0);
        
        if (valread <= 0) {
            printf("Server disconnected.\n");
            break;
        }

        printf("%s", buffer); // Print message from Server

        // 5. Check if Server is asking for a move
        if (strstr(buffer, "YOUR_TURN") != NULL) {
            char move[100];
            printf(">> Enter Action (DRAW / PASS): ");
            scanf("%s", move); // Blocking input
            
            // Send user input back to Server
            send(sock, move, strlen(move), 0);
        }
    }

    close(sock);
    return 0;
}