# ---- Build stage ----
FROM eclipse-temurin:21-jdk-alpine AS builder
WORKDIR /app
COPY pom.xml .
COPY src ./src
RUN apk add --no-cache maven \
    && mvn -q -DskipTests package

# ---- Runtime stage ----
FROM eclipse-temurin:21-jre-alpine
WORKDIR /app

# Install Clingo from Alpine community repo
RUN apk add --no-cache gringo

COPY --from=builder /app/target/clingo-container-1.0.0.jar app.jar

EXPOSE 8080
ENTRYPOINT ["java", "-jar", "app.jar"]
