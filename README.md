# clingo-container

Leichtgewichtige Web-Oberfläche für den [Clingo](https://potassco.org/clingo/) Answer-Set-Solver, gebaut mit Spring Boot 3 und Thymeleaf.

## Features

- Split-Editor (Code links, Output rechts)
- Vorausgefülltes Beispielprogramm (Graph-Coloring)
- Alle Antwortmengen (`clingo ... 0`)
- Dark Mode
- Responsiv (Mobile-tauglich)

## Voraussetzungen (lokal)

- Java 21+
- Maven
- `clingo` im `PATH` (z. B. `apt install gringo` / `brew install clingo`)

## Starten (lokal)

```bash
mvn spring-boot:run
```

Dann im Browser: <http://localhost:8080>

## Docker

```bash
# Build + Start
docker compose up --build

# Nur Image bauen
docker build -t clingo-web .
```

Dann im Browser: <http://localhost:8080>

## Beispielprogramm

Vorausgefüllt ist ein **Graph-Coloring**-Problem (4 Knoten, 3 Farben):

```prolog
node(1..4).
color(red;green;blue).
edge(1,2). edge(1,3). edge(2,3). edge(2,4). edge(3,4).

{ assign(N, C) : color(C) } = 1 :- node(N).
:- edge(X, Y), assign(X, C), assign(Y, C).

#show assign/2.
```

## Projektstruktur

```
.
├── src/main/java/io/github/ttsnap/clingo/
│   ├── ClingoApplication.java   # Spring Boot Entry Point
│   ├── ClingoController.java    # GET / und POST /run
│   └── ClingoRunner.java        # Clingo-Prozessaufruf
├── src/main/resources/
│   ├── templates/index.html     # Thymeleaf UI
│   └── application.properties
├── Dockerfile                   # Multi-stage Build
├── docker-compose.yml
└── pom.xml
```
