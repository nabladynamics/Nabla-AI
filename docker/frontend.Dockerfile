# Nabla AI — frontend image: static Vite build served by nginx.
#
# nginx also reverse-proxies /api (including the telemetry WebSocket) to the
# backend service, so the SPA is same-origin in the composed stack — exactly
# like the Vite dev proxy. Build from the repo root:
#   docker build -f docker/frontend.Dockerfile -t nabla-frontend .

FROM node:22-alpine AS build
WORKDIR /src
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci
COPY frontend/ ./
RUN npm run build

FROM nginx:1.27-alpine
COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /src/dist /usr/share/nginx/html
EXPOSE 80
