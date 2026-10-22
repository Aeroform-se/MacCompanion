.PHONY: up down build logs test dev client basilisk launch-client

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
	@chmod +x scripts/deploy-basilisk.sh
	./scripts/deploy-basilisk.sh

basilisk: client
	open -a "/Applications/Basillisk/BasiliskII.app"

launch-client: client
	./toolchain/Retro68-build/toolchain/bin/LaunchAPPL -e shared --shared-directory "/Applications/Basillisk/driveshare/MacCompanion/launch" client/build/MaciSpot.bin
