#ifndef WEBSERVER_H
#define WEBSERVER_H

// Registers HTTP routes and starts the server on port 80.
void webserverSetup();

// Services any pending HTTP clients. Call from loop().
void webserverLoop();

#endif
