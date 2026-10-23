.PHONY: up down build logs test dev client

up:
	docker-compose up -d --build

down:
	docker-compose down

build:
	docker-compose build

logs:
	docker-compose logs -f

test:
	cd middleware && npm test

dev:
	cd middleware && npm run dev

client:
	@chmod +x scripts/build-client.sh
	@if [ -f scripts/deploy-basilisk.sh ]; then chmod +x scripts/deploy-basilisk.sh; fi
	./scripts/build-client.sh
