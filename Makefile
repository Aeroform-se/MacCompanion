.PHONY: up down build logs test dev

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
