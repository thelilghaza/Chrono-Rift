CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# Uncomment the one LIBS line that matches your GUI choice:
LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt
# LIBS = $(shell sdl2-config --libs) -lrt
# LIBS = -lglfw -lGL -lrt
# LIBS = -lncurses -lrt

TARGETS = arbiter/arbiter hip/hip asp/asp gui/game_gui chrono_rift

all: clean $(TARGETS)
	@echo Build complete.

chrono_rift: launcher/main.cpp
	$(CXX) $(CXXFLAGS) launcher/main.cpp -o $@

arbiter/arbiter: arbiter/arbiter.cpp common/game_shared.h
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hip/hip: hip/hip.cpp common/game_shared.h
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

asp/asp: asp/asp.cpp common/game_shared.h
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

gui/game_gui: gui/game_gui.cpp
	$(CXX) $(CXXFLAGS) gui/game_gui.cpp -o $@ $(LIBS) -lX11

clean:
	rm -f arbiter/arbiter hip/hip asp/asp gui/game_gui chrono_rift

# Run all game binaries (expects Linux container or WSL with deps / display).
run: all
	./chrono_rift --single

run-pvp: all
	./chrono_rift --multiplayer pvp

run-coop: all
	./chrono_rift --multiplayer coop

stress-mp: all
	bash tests/stress_multiplayer.sh

docker:
	docker compose build

docker-build: docker

docker-run:
	docker compose up -d

# PDF needs pandoc (+ LaTeX engine on some installs). HTML fallback if PDF fails.
report: docs/REPORT.md
	@mkdir -p docs/screenshots
	@if command -v pandoc >/dev/null 2>&1; then \
		pandoc docs/REPORT.md -o docs/REPORT.pdf --toc -V geometry:margin=1in 2>/dev/null \
		|| pandoc docs/REPORT.md -o docs/REPORT.html --standalone --toc; \
		echo "Built docs/REPORT.pdf or docs/REPORT.html"; \
	else \
		echo "Install pandoc (e.g. apt install pandoc texlive-xetex) then re-run make report"; \
		cp -f docs/REPORT.md docs/REPORT.source.md 2>/dev/null || true; \
	fi

.PHONY: all clean run docker docker-build docker-run report
