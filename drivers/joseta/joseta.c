#include "joseta_internal.h"

////////////////////////////////////////////////////////////////////////////////
// GLOBAL VARIABLES ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static const char START_BYTE = 0xFF;
static const char ESCAPE_BYTE = 0xFE;
static int inside_escape = 0;  // set to 1 immediately after encountering an escape character, 0 otherwise

/* buffers */
static char joseta_serial_buffer[JOSETA_UART_BUF];
static char joseta_frame_buffer[JOSETA_BUFFER_SIZE];

/* driver state */
joseta_state_t joseta_state; 

/* thread stacks */
static char joseta_serial_thread_stack[JOSETA_SERIAL_STACK];
static char joseta_callback_thread_stack[JOSETA_CALLBACK_STACK];

void joseta_process_byte(char c);

////////////////////////////////////////////////////////////////////////////////
// INITIALIZATION FUNCTIONS ////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* initialize board and start polling thread */
void joseta_init(uint64_t rtc){
    joseta_state_init(rtc);
    joseta_timer_init();
    joseta_serial_thread_init();
    joseta_callback_thread_init();
    joseta_uart_init();
    joseta_board_init();
}

/* initialize uart device */
void joseta_uart_init(void){
    uart_init(JOSETA_UART, 9600U, joseta_serial_recv, 0, 0);
}

/* initialize timers */
void joseta_timer_init(void){

    /* set callback */
    timer_init(JOSETA_TIMER_NUM, 1, joseta_timer_cb);
    
    /* custom timer configuration */
    ROM_TimerDisable(JOSETA_TIMER_BASE, TIMER_A);
    ROM_TimerClockSourceSet(JOSETA_TIMER_BASE, TIMER_CLOCK_PIOSC);
    ROM_TimerConfigure(JOSETA_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC | TIMER_CFG_A_ACT_TOINTD);
    ROM_TimerPrescaleSet(JOSETA_TIMER_BASE, TIMER_A, JOSETA_TIMER_PRESCALE);
    ROM_TimerLoadSet(JOSETA_TIMER_BASE, TIMER_A, JOSETA_TIMER_MAX);
    ROM_TimerIntEnable(JOSETA_TIMER_BASE, TIMER_TIMA_MATCH);
    ROM_TimerIntEnable(JOSETA_TIMER_BASE, TIMER_TIMA_TIMEOUT);
    ROM_TimerEnable(JOSETA_TIMER_BASE, TIMER_A);
}

/* start chardev thread */
void joseta_serial_thread_init(void){
    
    ringbuffer_init(&joseta_state.serial_ringbuffer, joseta_serial_buffer,
                    JOSETA_UART_BUF
    );
    
    kernel_pid_t pid = thread_create(joseta_serial_thread_stack,
                                     sizeof(joseta_serial_thread_stack),
                                     PRIORITY_MAIN - 1,
                                     CREATE_STACKTEST | CREATE_SLEEPING,
                                     joseta_serial_loop,
                                     &joseta_state.serial_ringbuffer,
                                     "joseta_uart"
    );
    
    joseta_state.serial_pid = pid;
    thread_wakeup(pid);
}

/* start callback message-handling thread */
void joseta_callback_thread_init(void){
    
    ringbuffer_init(&joseta_state.frame_ringbuffer, joseta_frame_buffer,
                    JOSETA_BUFFER_SIZE
    );
    
    kernel_pid_t pid = thread_create(joseta_callback_thread_stack,
                                     sizeof(joseta_callback_thread_stack),
                                     PRIORITY_MAIN - 1,
                                     CREATE_STACKTEST | CREATE_SLEEPING,
                                     joseta_callback_loop,
                                     &joseta_state.frame_ringbuffer,
                                     "joseta_callback"
    );
    
    joseta_state.callback_pid = pid;
    thread_wakeup(pid);
}

/* initialize driver state */
void joseta_state_init(uint64_t rtc){
    
    /* clock */
    joseta_state.rtc = rtc;
    
    /* thread ids */
    joseta_state.serial_pid = KERNEL_PID_UNDEF;
    joseta_state.callback_pid = KERNEL_PID_UNDEF;
    
    /* default settings */
    joseta_state.purgethresh = JOSETA_DEFAULT_PURGETHRESH;
    joseta_state.callback = 0;
    
    /* read/write counters */
    joseta_state.current_frame_idx = 0;
    joseta_state.expected_frames = 0;
    joseta_state.frame_count = 0;
    
}

/* send initialization commands to board */
void joseta_board_init(void){
    joseta_state.fsm = JOSETA_FSM_INIT;
    printf("[joseta] sent reset command\n");
    joseta_send_reset();
}

/* set new epoch */
void joseta_finish_init(void){
    joseta_state.epoch = joseta_state.rtc;
    joseta_send_time(0);

    // delay, 1 second ish
    for (int i = 0; i < 12000000; i++);
    
    joseta_send_enable_streaming();
    printf("sent enable stream command\n");
}

/* set cb function */
void joseta_setcallback(joseta_cb_t fun, uint8_t mask){
    joseta_state.callback = fun;
    joseta_state.callback_mask = mask;
}

////////////////////////////////////////////////////////////////////////////////
// THREAD LOOPS  ///////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* process serial ringbuffer */
void *joseta_serial_loop(void *arg){
    ringbuffer_t *rb = (ringbuffer_t *) arg;
    msg_t m;
    while(1){
        msg_receive(&m);
          while (rb->avail) {
              char c = 0;
            unsigned state = disableIRQ();
            ringbuffer_get(rb, &c, 1);
            restoreIRQ(state);
            joseta_uart_byte(c);
        }  
    }
    return 0;
}

/* process callback events */
void *joseta_callback_loop(void *arg){
    ringbuffer_t *rb = (ringbuffer_t *) arg;
    msg_t m;
    
    while(1){
    
        msg_receive(&m);
        
        switch(m.type){
            
            // can eventually get rid of
            case JOSETA_CB_TIMER: {
                printf("[joseta] event: 1-minute timer expired\n");
                joseta_request_minute();
                break;
            }
            
            // not supposed to happen
            case JOSETA_CB_RESET: {
                printf("[joseta] event: initiate reset\n");
                joseta_state.pending_reset = true;
                joseta_request_minute();
                break;
            }
        
            // packet callback
            case JOSETA_CB_FRAME: {
                //printf("[joseta] event: processed frame\n");
                if(joseta_state.callback &&
                   joseta_state.callback_mask & JOSETA_CB_FRAME
                ){
                    joseta_df_t p;
                    unsigned state = disableIRQ();
                    unsigned offset = (rb->avail / sizeof(joseta_df_t)) - 1;
                    ringbuffer_peek_n_at(rb, (char*) &p, sizeof(joseta_df_t), sizeof(joseta_df_t) * offset);
                    restoreIRQ(state);
                    joseta_state.callback(m.type, &p, 1);
                }
                break;
            }
            
            // chunk callback
            case JOSETA_CB_PURGE: {
                printf("[joseta] event: purge\n");
                joseta_df_t p[JOSETA_BUFFER_COUNT];
                unsigned count = rb->avail / sizeof(joseta_df_t);
                
                /* purge */
                unsigned state = disableIRQ();
                ringbuffer_get(rb, (char*) p, count * sizeof(joseta_df_t));
                joseta_state.frame_count = 0;
                restoreIRQ(state);
            
                /* run callback if it exists and isn't masked */
                if(joseta_state.callback &&
                   joseta_state.callback_mask & JOSETA_CB_PURGE
                ){
                    joseta_state.callback(m.type, p, count);
                }
                
                printf("[joseta] purge complete\n");
                
                break;
                
            }
            
            case JOSETA_CB_ERROR: {
                printf("[joseta] event: error\n");
                if(joseta_state.callback &&
                   joseta_state.callback_mask & JOSETA_CB_ERROR
                ){
                    joseta_df_t p;
                    unsigned state = disableIRQ();
                    ringbuffer_get(rb, (char*) &p, sizeof(joseta_df_t));
                    restoreIRQ(state);
                    joseta_state.callback(m.type, &p, 1);
                }
                break;
            }
            
            default: {
                printf("[joseta] unknown event type %d\n", m.type);
            }
        
        }
        
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// INPUT-PROCESSING FUNCTIONS  /////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* handle incoming character from uart */
void joseta_serial_recv(void *arg, char c){
    msg_t m;
    m.type = 0;
    ringbuffer_add_one(&joseta_state.serial_ringbuffer, c);
    msg_send_int(&m, joseta_state.serial_pid);
}

/* wrapper around char processor that handles special circumstances */
void joseta_uart_byte(char c) {
    if (inside_escape == 1) {
        inside_escape = 0;
        joseta_process_byte(c);
    } else if (c == ESCAPE_BYTE) {
        inside_escape = 1;
    } else if (c == START_BYTE) {
        /*if (joseta_state.current_frame_idx != 15) {
            printf("Got an incomplete byte; re-buffering\n");
            printf("The amount of bytes buffered was %d\n", joseta_state.current_frame_idx);

            printf("[joseta] raw frame: ");
            for(int i = 0; i < joseta_state.current_frame_idx; i++){
                printf("%02x ", joseta_state.current_frame[i]);
            }
            printf("\n");
        }*/
        joseta_state.current_frame_idx = 0;
    } else {
        joseta_process_byte(c);
    }
}

/* process buffered character */
void joseta_process_byte(char c){

    joseta_state.current_frame[joseta_state.current_frame_idx++] = c;

    /* if frame complete */
    if(joseta_state.current_frame_idx == JOSETA_RAW_FRAME_SIZE){

        /* reset counter */
        joseta_state.current_frame_idx = 0;
        
        switch(joseta_state.fsm){
            
            /* data frame was expected */
            case JOSETA_FSM_READ: {
                
                /* process this frame */
                //printf("[joseta] recv frame\n");
                joseta_process_frame();

                // TODO: handle joseta_state.pending_reset
                // Previously, if there was a pending reset, this waited until all the frames were in to reset
                
                break;
            }
            
            /* data frame was a request for epoch */
            case JOSETA_FSM_INIT: {
                printf("[joseta] board has reset, setting new epoch\n");
                joseta_finish_init();
                joseta_state.fsm = JOSETA_FSM_READ;
                break;
            }
            
            /* undefined fsm state */
            default: {
                printf("[joseta] driver in bad state (%d)\n", joseta_state.fsm);
                break;
            }
            
        }        
    }
}

uint16_t joseta_calc_crc(uint8_t* data_p, uint8_t length){
        uint8_t x;
        uint16_t crc = 0; //0xFFFF;
        while (length--){
                x = crc >> 8 ^ *data_p++;
                x ^= x>>4;
                crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x <<5)) ^ ((uint16_t)x);
        }
        return crc;
}

/* check crc from buffered data */
bool joseta_verify_crc(void){
    joseta_raw_frame_t *frame = (joseta_raw_frame_t*) joseta_state.current_frame;
    uint16_t my_crc = joseta_calc_crc((uint8_t *) frame, JOSETA_RAW_FRAME_SIZE - 2);
    /*if (my_crc == frame->crc) {
	    printf("\nCRC succeeded!\n");
    } else {
        printf("\nCRC failed ya dummy!\n");
        printf("TI CRC = %d\n", my_crc);
        printf("Python CRC = %d\n", frame->crc); 
    }
    return true;*/
    return (my_crc == frame->crc);
}

/* process current buffered frame */
void joseta_process_frame(void){
    joseta_raw_frame_t *frame = (joseta_raw_frame_t*) joseta_state.current_frame;
    joseta_df_t parsed;
    msg_t m1, m2;
    
    if(!joseta_verify_crc()){
        printf("[joseta] discarding frame with bad crc\n");
        for(int i = 0; i < JOSETA_RAW_FRAME_SIZE; i++){
                printf("%02x ", joseta_state.current_frame[i]);
        }
    }
    else{
        printf("[joseta] raw frame: ");
        for(int i = 0; i < JOSETA_RAW_FRAME_SIZE; i++){
                printf("%02x ", joseta_state.current_frame[i]);
        }
        printf("\n                    flags=%02x, voltage=%u, current=%u, phase=%d, temp=%u, time=%lu, reserved=%u, err=%u, crc=%04x\n",
            frame->flags,
            frame->voltage,
            frame->current,
            frame->phase,
            frame->temperature,
            frame->timestamp,
            frame->reserved,
            frame->error,
            frame->crc
    );
    
        /* parse frame */
        parsed.occupancy = frame->flags & JOSETA_FLAG_OCCUPANCY;
        parsed.relay     = frame->flags & JOSETA_FLAG_RELAY;
        parsed.voltage   = frame->voltage;
        parsed.current   = frame->current;
        parsed.phase     = frame->phase;
        parsed.temp      = frame->temperature;
        parsed.time      = frame->timestamp + joseta_state.epoch;
        parsed.error     = frame->error;
        
        /* pass to handler thread */
        ringbuffer_add(&joseta_state.frame_ringbuffer,
                       (const char *) &parsed,
                       sizeof(joseta_df_t)
        );
        m1.type = JOSETA_CB_FRAME;
        msg_send(&m1, joseta_state.callback_pid);
        
        /* increment frame counter, purge buffer if necessary */
        if((++joseta_state.frame_count) >= joseta_state.purgethresh){
            m2.type = JOSETA_CB_PURGE;
            msg_send(&m2, joseta_state.callback_pid);
        }
        
    }
}

////////////////////////////////////////////////////////////////////////////////
// REQUEST CONTROL  ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* send serial frame with given type and payload */
void joseta_send_frame(uint8_t type, uint8_t payload){
    /* calc checksum */
    uint8_t cs = 0x00;
    cs = (cs + (type << 4)) & 0xff;
    cs = (cs + payload    ) & 0xff;
    cs = 0xff - cs;
    
    /* send frame */
    uart_write_blocking(JOSETA_UART, type << 4);
    uart_write_blocking(JOSETA_UART, payload);
    uart_write_blocking(JOSETA_UART, cs);
}

/* send request for data frame(s) */
void joseta_send_dreq(uint8_t addr){
    joseta_state.fsm = JOSETA_FSM_READ;
    joseta_send_frame(0x1, addr << 4);
}

/* request one minute worth of data */
void joseta_request_minute(void){
    joseta_state.expected_frames = 60;
    joseta_send_dreq(0xf);
}

/* send time */
void joseta_send_time(uint8_t time){
    joseta_send_frame(0x04, 0x80 | time);
}

/* reset the sensor device */
void joseta_send_reset(void){
    joseta_send_frame(0x4, 0);
}

/* tick rtc and trigger request every minute */
void joseta_timer_cb(int arg){
    static uint32_t ticks = 0;
    static bool drift = false;
    if((ticks = (ticks + 1) % JOSETA_TIMER_INTERVAL) == 0){
        joseta_state.rtc++;
        if(drift){
            drift = false;
        }
        else{
            msg_t m;
            if(joseta_state.rtc % JOSETA_TIMER_DRIFT == 0){
                joseta_state.rtc--;
                drift = true;
            }
            if((joseta_state.rtc - joseta_state.epoch) % 86400 == 0){
                msg_send(&m, joseta_state.callback_pid);
                m.type = JOSETA_CB_RESET;
            }
        }
    }
}

void joseta_send_enable_streaming(void){
    joseta_send_frame(0x5, 128);  // flag is 1 for disable, rate is 0 for unchanged
}

void joseta_send_disable_streaming(void){
    joseta_send_frame(0x5, 0);  // flag is 0 for disable, rate is 0 for unchanged
}

void joseta_send_stream_rate(uint8_t seconds) {
    joseta_send_frame(0x5, 128 | seconds); // flag is 1 for enable, rate is the value of seconds, with a max of 127.
}
    
