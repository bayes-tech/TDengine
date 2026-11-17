services:
  tdengine:
    image: tdengine-community:3.4.3.9-ubuntu24.04-x64
    platform: linux/amd64
    pull_policy: never
    restart: unless-stopped
    stop_grace_period: 60s
    environment:
      # Advertised endpoint. Clients must resolve this name to the Docker host.
      TAOS_FQDN: "${TDENGINE_FQDN:-localhost}"
      TAOS_FIRST_EP: "${TDENGINE_FQDN:-localhost}:${TDENGINE_PORT:-6030}"
      TAOS_SERVER_PORT: "${TDENGINE_PORT:-6030}"
    ports:
      # Use the same port inside and outside the container for native clients.
      - "${TDENGINE_BIND_IP:-127.0.0.1}:${TDENGINE_PORT:-6030}:${TDENGINE_PORT:-6030}/tcp"
    volumes:
      - tdengine-data:/var/lib/taos
      - tdengine-logs:/var/log/taos
    healthcheck:
      test:
        - CMD-SHELL
        - >-
          taos -h 127.0.0.1 -P "$${TAOS_SERVER_PORT}" -s 'SHOW DNODES;' 2>&1
          | grep -Eq '\|[[:space:]]*ready[[:space:]]*\|'
      interval: 10s
      timeout: 8s
      retries: 6
      start_period: 30s

volumes:
  tdengine-data:
  tdengine-logs:
