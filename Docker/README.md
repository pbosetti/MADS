# Docker services

It is possible to launch a set of docker containers providing the MongoDB database, the broker and the logger agents without installing nor compiling any software. The following steps will guide you through the process.

## First step: create the config files

To have an help:

```bash
docker run --rm p4010/miroscic
```

The command can be used to create the necessary template files:

- `miroscic.ini` configuration file (to be possibly edited)
- `compose.yml` docker-compose file, needed for launching and coordinating services (no need to edit)

Act as follows:

```bash
mkdir miroscic                                 # only if needed
cd miroscic
mkdir db                                       # will hold MongoDB data
docker pull p4010/miroscic                     # fetch the image from internet
docker run --rm p4010/miroscic ini > miroscic.ini    # creates the configuration file
docker run --rm p4010/miroscic compose > compose.yml # creates the docker-compose file
```

If needed, update the `miroscic.ini` file with the desired configuration.

**Note:** on Windows powershell, the default character encoding is UTF-16. Consequently, the last two commands produces files that are UTF-16 encoded. This is a problem, for the file `miroscic.ini` is eventually read by a process running in the Linux container, which expects a UTF-8 text file. To create the INI file on Windows, thus, use the following commands:

```powershell
docker run --rm p4010/miroscic ini | out-file miroscic.ini -encoding utf8
docker run --rm p4010/miroscic compose | out-file compose.yml -encoding utf8
```

## Second step: launch the services

To launch the services in background, just run the following command:

```bash
docker compose up -d
```

To stop the services, run the following command:

```bash
docker compose down
```

## Networking

Note that the three services are connected to each other via internal, hidden networks and IPs. From outside docker, only the broker and the mongo instance need to be reached, and they are exposed on the host machine on the same ports specified in the configuration file.

Consequently, to run other agents, these have to search for the broker-provided settings file using `-s tcp://<host machine IP>:<broker settings port>`, where `host machine IP` is the IP or address name of the machine running the containers, and the port is the `broker:settings_address` value set in `miroscic.ini`. Agents working on the same machine can specify `localhost` or simply omit the `-s` option, since `localhost` is the default value.

Likewise, a MongoDB Compass client running on the docker host machine can connect to the database using the `mongodb://localhost:27017` connection string.

## Developer notes

To build a multi-arch image:

```bash
docker buildx build -t p4010/mads --platform=linux/arm64,linux/amd64 --push .
```

If any error, create a new buildx instance with:

```bash
docker buildx create --name multiarch --driver docker-container --use
```
