kilo: kilo.c  # says that we want to build kilo, which DEPENDS on kilo.c
	$(CC) kilo.c -o kilo -Wall -Wextra -pedantic -std=c99