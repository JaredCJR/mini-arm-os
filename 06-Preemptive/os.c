#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "reg.h"
#include "asm.h"
#include "semihost/host.h"


/* Size of our user task stacks in words */
#define STACK_SIZE	256

/* Number of user task */
#define TASK_LIMIT	5

/* USART TXE Flag
 * This flag is cleared when data is written to USARTx_DR and
 * set when that data is transferred to the TDR
 */
#define USART_FLAG_TXE	((uint16_t) 0x0080)

/* reverse:  reverse string s in place */
void reverse(char s[])
{
	int i, j;
	char c;

	for (i = 0, j = strlen(s) - 1; i < j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}
/* itoa:  convert n to characters in s
 * K&R implementation
 * http://en.wikibooks.org/wiki/C_Programming/C_Reference/stdlib.h/itoa
 */
void itoa(int n, char s[])
{
	int i, sign;

	if ((sign = n) < 0)  /* record sign */
		n = -n;          /* make n positive */
	i = 0;
	do {       /* generate digits in reverse order */
		s[i++] = n % 10 + '0';   /* get next digit */
	} while ((n /= 10) > 0);     /* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	reverse(s);
}




void usart_init(void)
{
	*(RCC_APB2ENR) |= (uint32_t)(0x00000001 | 0x00000004);
	*(RCC_APB1ENR) |= (uint32_t)(0x00020000);

	/* USART2 Configuration, Rx->PA3, Tx->PA2 */
	*(GPIOA_CRL) = 0x00004B00;
	*(GPIOA_CRH) = 0x44444444;
	*(GPIOA_ODR) = 0x00000000;
	*(GPIOA_BSRR) = 0x00000000;
	*(GPIOA_BRR) = 0x00000000;

	*(USART2_CR1) = 0x0000000C;
	*(USART2_CR2) = 0x00000000;
	*(USART2_CR3) = 0x00000000;
	*(USART2_CR1) |= 0x2000;
}

typedef enum TASK_STATE {
	WAITING,
	RUNNING,
	READY,
	SUSPENDED,
	CREATED
} TASK_STATE;

/*
 * The Task_scheduler is a state used for checking the second level priority queue
 */
typedef enum TASK_SCHEDULING_STATE {
	SCHEDULED,
	UNSCHEDULED
} TASK_SCHEDULING_STATE;

typedef struct Task {
	const char* task_name;
	unsigned int priority;/* the number bigger,then the priority is higher.This is the current priority*/
	unsigned int *task_address;
	unsigned int user_stack[STACK_SIZE];
	TASK_STATE state;
	TASK_SCHEDULING_STATE sch_state;
} xTask;

xTask user_task[TASK_LIMIT];
static unsigned int scheduler_initial_flag = 1;

void print_str(const char *str)
{
	while (*str) {
		while (!(*(USART2_SR) & USART_FLAG_TXE));
		*(USART2_DR) = (*str & 0xFF);
		str++;
	}
}


void print_int(int n)
{
	char buf[] = "\0";
	itoa(n, buf);
	print_str(buf);
}


void delay(int count)
{
	count *= 50000;
	while (count--);
}

/* Exception return behavior */
#define HANDLER_MSP	0xFFFFFFF1
#define THREAD_MSP	0xFFFFFFF9
#define THREAD_PSP	0xFFFFFFFD

/* Initilize user task stack and execute it one time */
/* XXX: Implementation of task creation is a little bit tricky. In fact,
 * after the second time we called `activate()` which is returning from
 * exception. But the first time we called `activate()` which is not returning
 * from exception. Thus, we have to set different `lr` value.
 * First time, we should set function address to `lr` directly. And after the
 * second time, we should set `THREAD_PSP` to `lr` so that exception return
 * works correctly.
 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Babefdjc.html
 */
unsigned int *create_task(unsigned int *stack, void (*start)(void), unsigned int priority, const char* name, size_t task_count)
{
	static int first = 1;

	stack += STACK_SIZE - 32; /* End of stack, minus what we are about to push */
	if (first) {
		stack[8] = (unsigned int) start;
		first = 0;
	} else {
		stack[8] = (unsigned int) THREAD_PSP;
		stack[15] = (unsigned int) start;
		stack[16] = (unsigned int) 0x01000000; /* PSR Thumb bit */
	}
	user_task[task_count].state = CREATED;
	user_task[task_count].priority = priority;
	user_task[task_count].task_name = name;
	stack = activate(stack);
	user_task[task_count].state = READY;
	user_task[task_count].sch_state = UNSCHEDULED;
	return stack;
}

/*
 * use Task_suspend() or Task_resume() or Task_modify_priority()  like:
 * xTask *ptr = &user_task[i];        //i is the task that you want to do something!
 * Task_suspend(ptr);
 * or
 * Task_resume(ptr);
 * or
 * Task_modify_priority(ptr,priority_number);
 */
void Task_suspend(xTask *task)
{
	task->state = SUSPENDED;
	print_str("\n");
	print_str(task->task_name);
	print_str(" is suspended!\n");
	*SYSTICK_VAL = 0;
	syscall();
}

void Task_resume(xTask *task)
{
	task->state = READY;
	print_str("\n");
	print_str(task->task_name);
	print_str(" resume to READY state!\n");
	*SYSTICK_VAL = 0;
	syscall();
}

void Task_modify_priority(xTask *task, unsigned int pri)
{
	task->priority = pri;
	scheduler_initial_flag = 1;
	print_str("\nModify priority for ");
	print_str(task->task_name);
	print_str(" : ");
	print_int(task->priority);
	print_str("\n");
}


void Task_scheduler(xTask tasks[], size_t created_task_number)
{
	*SYSTICK_VAL = 0;
	size_t current_task = 0;
	unsigned int max = 0;
	unsigned int i = 0;
	unsigned int j = 0;
	scheduler_initial_flag = 1;
	xTask *pTask[created_task_number];

	while (1) {
		current_task = 0;
		max = 0;
		i = 0;//level 1:priority based
		if ((j == created_task_number) || (scheduler_initial_flag == 1)) { //level 2: round robin
			j = 0;
			for (; j < created_task_number; j++) {
				tasks[j].sch_state = UNSCHEDULED;
			}
			scheduler_initial_flag = 0;
			j = 0;
		}

		for (; i < created_task_number; i++) { //level 1
			if (tasks[i].priority > max && (tasks[i].state == READY) && (tasks[i].sch_state == UNSCHEDULED)) {
				max = tasks[i].priority;
				current_task = i;
			}
		}
		pTask[j] = &tasks[current_task];//level 2
		pTask[j]->sch_state = SCHEDULED;

		print_str("OS: Activate next task\n");
		if (pTask[j]->state == READY) {
			pTask[j]->state = RUNNING;
			pTask[j]->task_address = activate(pTask[j]->task_address);//activate
		}
		if (pTask[j]->state == RUNNING) { //if  the state is changed during the process modify its running time
			pTask[j]->state = READY;
		}

		j++;
		print_str("OS: Back to OS\n");

	}
}

void semihost_logger(void)
{
	int handle , error;
	/*char output[512] = {0};*/
	char *buf;
	print_str("semihost_logger Created!\n");
	handle = host_action(SYS_SYSTEM, "mkdir -p output");
	handle = host_action(SYS_SYSTEM, "touch output/syslog");
	handle = host_action(SYS_OPEN, "output/syslog", 4);
	print_int(handle);
	print_str("\n");
	if (handle == -1) {
		print_str("Open file error!\n");
	}
	syscall();
	while (1) {
		buf = "Test for semihost!\n";
		/*memcpy(output, (char *)buf, strlen((char*)buf));*/
		print_str("semihost_logger is logging!\n");
		error = host_action(SYS_WRITE, handle, (void*)buf, strlen((char *)buf));
		if (error != 0) {
			print_str("Write file error!\n");
			host_action(SYS_CLOSE, handle);
			return;
		}
		syscall();
	}
	host_action(SYS_CLOSE, handle);
}

void task1_func(void)
{
	print_str("task1: Created!\n");
	syscall();
	int test = 0;
	xTask *ptr = &user_task[2];
	while (1) {
		print_str("Running...");
		print_str(user_task[1].task_name);
		print_str("\n");
		delay(1000);

		test++;
		if (test == 10) {
			Task_modify_priority(ptr, 20);
			print_str("task 2 gets highest priority!");
		}
		if (test == 15) {
			Task_suspend(ptr);
		}
	}
}


void task2_func(void)
{
	print_str("task2: Created!\n");
	syscall();
	while (1) {
		print_str("Running...");
		print_str(user_task[2].task_name);
		print_str("\n");
		delay(1000);
	}
}


void task3_func(void)
{
	print_str("task3: Created!\n");
	syscall();
	while (1) {
		print_str("Running...");
		print_str(user_task[3].task_name);
		print_str("\n");
		delay(1000);
	}
}



int main(void)
{
	size_t task_count = 0;

	usart_init();

	print_str("OS: Starting...\n");
	print_str("OS: First create semihost_logger !\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &semihost_logger, 0, "semihost_logger!", task_count);
	task_count += 1;
	print_str("OS: Back to OS, create task 1\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task1_func, 1, "task_name_1", task_count);
	task_count += 1;

	print_str("OS: Back to OS, create task 2\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task2_func, 10, "task_name_2", task_count);
	task_count += 1;

	print_str("OS: Back to OS, create task 3\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task3_func, 14, "task_name_3", task_count);
	task_count += 1;

	/* SysTick configuration */
	*SYSTICK_LOAD = 7200000;
	*SYSTICK_VAL = 0;
	*SYSTICK_CTRL = 0x07;
	print_str("Scheduler start!\n");
	Task_scheduler(user_task, task_count); /*priority based with round-robin 2 level scheduler*/
	return 0;
}
