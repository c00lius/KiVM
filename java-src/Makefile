JAVAC := javac

JAVAC := /Library/Java/JavaVirtualMachines/jdk1.8.0_241.jdk/Contents/Home/bin/javac

DIR := $(shell pwd)
SRC := $(shell find . -type f -name '*.java')
OBJ := $(SRC:.java=.class)

all: $(OBJ)
	@echo " Done"

%.class: %.java
	@echo "[Compiling] $<"
	@$(JAVAC) -d ../java-out -encoding utf-8  $<
