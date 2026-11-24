const express = require("express");
const http = require("http");
const { Server } = require("socket.io");
const path = require("path");

const app = express();
const server = http.createServer(app);
const io = new Server(server);

app.use(express.static(path.join(__dirname, "dashboard")));

io.on("connection", (socket) => {
    console.log("Dashboard connected");

    socket.on("disconnect", () => {
        console.log("Dashboard disconnected");
    });
});

server.listen(3000, "127.0.0.1", () => {
    console.log("True Goal running on port 3000");
});