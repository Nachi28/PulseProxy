FROM gcc:14 AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPP_WARNINGS_AS_ERRORS=ON \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
COPY --from=build /src/build/pulseproxy /usr/local/bin/pulseproxy
EXPOSE 8080
ENTRYPOINT ["pulseproxy"]
CMD ["--listen-port", "8080", "--upstream-host", "127.0.0.1", "--upstream-port", "8081"]
