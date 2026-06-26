# clingo-container

Leichtgewichtige Web-Oberfläche für den [Clingo](https://potassco.org/clingo/) Answer-Set-Solver.
Zwei Implementierungen mit identischer UI und Funktionalität:

| | `java/` | `native/` |
|---|---|---|  
| Stack | Spring Boot 3 + Thymeleaf + JVM 21 | C + libmicrohttpd |
| Image-Größe | ~250 MB | ~15 MB |
| RAM (idle) | ~120–180 MB | ~3–5 MB |
| Startup | ~3–5 s | < 10 ms |
| Port | 8080 | 8080 |

## Features (beide Versionen)

- Split-Editor (Code links, Output rechts)
- Vorausgefülltes Beispielprogramm (Graph-Coloring)
- Alle Antwortmengen (`clingo ... 0`)
- Dark Mode
- Responsiv (Mobile-tauglich)
- Timeout nach 30 Sekunden

---

## Java-Version (`java/`)

### Voraussetzungen (lokal)
- Java 21+, Maven, `clingo` im `PATH`

### Lokal starten
```bash
cd java
mvn spring-boot:run
```

### Docker
```bash
cd java
docker compose up --build
```

---

## Native-Version (`native/`)

Pure-C-Implementierung mit [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/).
Kein JVM, kein Framework — startet in Millisekunden, ~3 MB RAM.

### Docker (empfohlen)
```bash
cd native
docker compose up --build
```

Dann im Browser: <http://localhost:8080>

### Lokal bauen (Linux)
```bash
# Abhängigkeiten (Debian/Ubuntu)
sudo apt install gcc libmicrohttpd-dev clingo

# Bauen
cd native
gcc -Os -s -o clingo-web server.c -lmicrohttpd

# Starten
./clingo-web
```

### Docker-Besonderheiten
- Multi-Stage Build: Builder-Stage kompiliert, Runtime-Stage enthält nur Binary + clingo
- `read_only: true` Container-Filesystem
- `tmpfs` auf `/tmp` für Clingo-Temp-Dateien
- `no-new-privileges` Security-Flag
- Non-root User (uid 1001)

---

## Projektstruktur

```
.
├── java/                          # Spring Boot Implementierung (original)
│   ├── src/main/java/io/github/ttsnap/clingo/
│   │   ├── ClingoApplication.java
│   │   ├── ClingoController.java
│   │   └── ClingoRunner.java
│   ├── src/main/resources/
│   │   ├── templates/index.html
│   │   └── application.properties
│   ├── Dockerfile
│   ├── docker-compose.yml
│   └── pom.xml
│
└── native/                        # Pure-C Implementierung (neu)
    ├── server.c                   # Kompletter HTTP-Server + Clingo-Runner
    ├── Dockerfile                 # Multi-Stage Alpine Build
    └── docker-compose.yml
```

## Beispielprogramm

```prolog
node(1..4).
color(red;green;blue).
edge(1,2). edge(1,3). edge(2,3). edge(2,4). edge(3,4).

{ assign(N, C) : color(C) } = 1 :- node(N).
:- edge(X, Y), assign(X, C), assign(Y, C).

#show assign/2.
```
