CC ?= cc
test: test_film_filter
	./test_film_filter
test_film_filter: test_film_filter.c ../main/film_filter.c ../main/film_filter.h ../main/focus.c ../main/focus.h
	$(CC) -Wall -Wextra -O1 -fsanitize=address,undefined -o $@ test_film_filter.c ../main/film_filter.c ../main/focus.c
clean:
	rm -f test_film_filter
