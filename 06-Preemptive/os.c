#include <stddef.h>
#include <stdint.h>
#include "reg.h"
#include "asm.h"


/* Size of our user task stacks in words */
#define STACK_SIZE	256

/* Number of user task */
#define TASK_LIMIT	5

/* USART TXE Flag
 * This flag is cleared when data is written to USARTx_DR and
 * set when that data is transferred to the TDR
 */
#define USART_FLAG_TXE	((uint16_t) 0x0080)

void usart_init(void)
{
	*(RCC_APB2ENR) |= (uint32_t) (0x00000001 | 0x00000004);
	*(RCC_APB1ENR) |= (uint32_t) (0x00020000);

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

typedef enum Task_state{
    	waiting,
	running,
	ready,
	suspended,
	created
} task_state;

typedef struct Task{
    const char* task_name;
    unsigned int priority;//the number bigger,then the priority is higher
    unsigned int *task_address;
    unsigned int user_stack[STACK_SIZE];
    task_state state;
} xTask;

	xTask user_task[TASK_LIMIT];

void print_str(const char *str)
{
	while (*str) {
		while (!(*(USART2_SR) & USART_FLAG_TXE));
		*(USART2_DR) = (*str & 0xFF);
		str++;
	}
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
unsigned int *create_task(unsigned int *stack, void (*start)(void), unsigned int priority,const char* name,size_t task_count)
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
	user_task[task_count].state = created;
	user_task[task_count].priority = priority;
	user_task[task_count].task_name = name;
	stack = activate(stack);
	user_task[task_count].state = ready;
	return stack;
}

void Task_suspend(xTask *task)
{
    task->state = suspended;
    print_str(task->task_name);
    print_str(" is suspended!\n");
    *SYSTICK_VAL = 0;
    syscall();
}

void Task_resume(xTask *task)
{
    task->state = ready;
    print_str(task->task_name);
    print_str(" resume to ready state!\n");
    *SYSTICK_VAL = 0;
    syscall();
}


void Task_scheduler(xTask tasks[],size_t created_task_number)
{
    size_t current_task = 0;
    unsigned int max = 0;
    int i = 0;
    while(1)
    {
	current_task = 0;
	max = tasks[0].priority;
	i = 0;
	for(; i < created_task_number; i++)
	{
	    if(tasks[i].priority > max && (tasks[i].state == ready))
	    {
		max = tasks[i].priority;
		current_task = i;
	    }
	}
		print_str("OS: Activate next task\n");
		if(tasks[current_task].state == ready)
		{
		tasks[current_task].state = running;
		tasks[current_task].task_address = activate(tasks[current_task].task_address);
		}
		if(tasks[current_task].state == running)
		{
		    tasks[current_task].state = ready;
		}
		print_str("OS: Back to OS\n");
    }
}



void task0_func(void)
{
	print_str("task0: Created!\n");
	syscall();
	xTask *ptr = &user_task[1];
	while (1) {
		print_str("Running...");
		print_str(user_task[0].task_name);
		print_str("\n");
		delay(1000);
		Task_resume(ptr);
	}
}

void task1_func(void)
{
	print_str("task1: Created!\n");
	syscall();
	xTask *ptr = &user_task[1];
	while (1) {
		print_str("Running...");
		print_str(user_task[1].task_name);
		print_str("\n");
		delay(1000);
		Task_suspend(ptr);
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
	//size_t current_task;

	usart_init();

	print_str("OS: Starting...\n");
	print_str("OS: First create task 0\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task0_func, 2, "task_name_0", task_count);
	task_count += 1;
	print_str("OS: Back to OS, create task 1\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task1_func, 12, "task_name_1", task_count);
	task_count += 1;

	/*print_str("OS: Back to OS, create task 2\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task2_func, 10, "task_name_2", task_count);
	task_count += 1;

	print_str("OS: Back to OS, create task 3\n");
	user_task[task_count].task_address = create_task(user_task[task_count].user_stack, &task3_func, 14, "task_name_3", task_count);
	task_count += 1;*/

	/* SysTick configuration */
	*SYSTICK_LOAD = 7200000;
	*SYSTICK_VAL = 0;
	*SYSTICK_CTRL = 0x07;
	print_str("Scheduler start!\n");
	Task_scheduler(user_task,task_count);//priority based scheduler
	return 0;
}
