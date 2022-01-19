compile: 
	@gcc -ggdb3 $(shell find ./ -name "*.c")
		-lpthread \
		-o build/test

clean:
	@rm -rf $(shell find build/)
	@mkdir build

threadsanitize: 
	@gcc -ggdb3 -fsanitize=thread $(shell find ./ -name "*.c")
		-lpthread \
		-o build/test	

perf: 
	@gcc -ggdb3 $(shell find ./ -name "*.c")
		-lpthread \
		-o build/test
	@build/test 10

BKL: 
	@gcc -ggdb3 -DBKL $(shell find ./ -name "*.c")
		-lpthread \
		-o build/test
	@build/test 10

testall: 
	@gcc -ggdb3 $(shell find ./ -name "*.c") \
		-DTEST -DDEBUG \
		-lpthread -o build/test

	@echo "testing ...     single-thread | small_memory"
	@build/test 1
	@echo "============================================"

	@echo "testing ...       muti-thread | small_memory"
	@build/test 2
	@echo "============================================"

	@echo "testing ...       single-thread | big_memory"
	@build/test 3
	@echo "============================================"

	@echo "testing ...         muti-thread | big_memory"
	@build/test 4
	@echo "============================================"

	@echo "testing ...       single-thread | big_memory"
	@build/test 5
	@echo "============================================"

	@echo "testing ...         muti-thread | mix_memory"
	@build/test 6
	@echo "============================================"

	@echo "testing ...    single-thread | restrict_mode"
	@build/test 5
	@echo "============================================"

	@echo "testing ...      muti-thread | restrict_mode"
	@build/test 6
	@echo "============================================"