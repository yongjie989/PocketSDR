/*----------------------------------------------------------------------------*/
/**
 *  Pocket SDR - SDR Device Functions.
 *
 *  Author:
 *  T.TAKASU
 *
 *  History:
 *  2021-10-20  0.1  new
 *  2022-01-04  0.2  support CyUSB on Windows
 *  2022-01-13  1.0  rise process/thread priority for Windows
 *
 */
#include "pocket.h"
#ifdef CYUSB
#include "avrt.h"
#endif

/* constants and macros ------------------------------------------------------*/
#define TO_TRANSFER     3000    /* USB transfer timeout (ms) */

/* quantization lookup table -------------------------------------------------*/
static int8_t LUT[2][2][256];

/* generate quantization lookup table ----------------------------------------*/
static void gen_LUT(void)
{
    static const int8_t val[] = {+1, +3, -1, -3}; /* 2bit, sign + magnitude */
    int i;
    
    if (LUT[0][0][0]) return;
    
    for (i = 0; i < 256; i++) {
        LUT[0][0][i] = val[(i>>0) & 0x3]; /* CH1 I */
        LUT[0][1][i] = val[(i>>2) & 0x3]; /* CH1 Q */
        LUT[1][0][i] = val[(i>>4) & 0x3]; /* CH2 I */
        LUT[1][1][i] = val[(i>>6) & 0x3]; /* CH2 Q */
    }
}

/* read sampling type --------------------------------------------------------*/
static int read_sample_type(sdr_dev_t *dev)
{
    uint8_t data[4];
    int i;
    
    for (i = 0; i < SDR_MAX_CH; i++) {
        /* read MAX2771 ENIQ field */
        if (!sdr_usb_req(dev->usb, 0, SDR_VR_REG_READ, (uint16_t)((i << 8) + 1),
                 data, 4)) {
           return 0;
        }
        dev->IQ[i] = ((data[0] >> 3) & 1) ? 2 : 1; /* I:1,IQ:2 */
    }
    return 1;
}

#ifdef CYUSB

/* get bulk transfer endpoint ------------------------------------------------*/
static sdr_ep_t *get_bulk_ep(sdr_usb_t *usb, int ep)
{
    int i;
    
    for (i = 0; i < usb->EndPointCount(); i++) {
        if (usb->EndPoints[i]->Attributes == 2 &&
            usb->EndPoints[i]->Address == ep) {
            return (sdr_ep_t *)usb->EndPoints[i];
        }
    }
    fprintf(stderr, "No bulk end point ep=%02X\n", ep);
    return NULL;
}

/* read buffer ---------------------------------------------------------------*/
static uint8_t *read_buff(sdr_dev_t *dev)
{
    int rp = dev->rp;
    
    if (rp == dev->wp) {
        return NULL;
    }
    if (rp == (dev->wp + 1) % SDR_MAX_BUFF) {
        fprintf(stderr, "bulk transfer buffer overflow\n");
    }
    dev->rp = (rp + 1) % SDR_MAX_BUFF;
    return dev->buff[rp];
}

/* rise process/thread priority ----------------------------------------------*/
static void rise_pri(void)
{
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        fprintf(stderr, "SetPriorityClass error (%d)\n", (int)GetLastError());
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        fprintf(stderr, "SetThreadPriority error (%d)\n", (int)GetLastError());
    }
    DWORD task = 0;
    HANDLE h = AvSetMmThreadCharacteristicsA("Capture", &task);
    
    if (h == 0) {
        fprintf(stderr, "AvSetMmThreadCharacteristicsA error (%d)\n",
            (int)GetLastError());
    }
    else if (!AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL)) {
        fprintf(stderr, "AvSetMmThreadPriority error (%d)\n",
            (int)GetLastError());
    }
}

/* event handler thread ------------------------------------------------------*/
static DWORD WINAPI event_handler(void *arg)
{
    sdr_dev_t *dev = (sdr_dev_t *)arg;
    uint8_t *ctx[SDR_MAX_BUFF] = {0};
    OVERLAPPED ov[SDR_MAX_BUFF] = {0};
    long len = SDR_SIZE_BUFF;
    int i;
    
    /* rise process/thread priority */
    rise_pri();
    
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        ov[i].hEvent = CreateEvent(NULL, false, false, NULL);
        ctx[i] = dev->ep->BeginDataXfer(dev->buff[i], len, &ov[i]); 
    }
    for (i = 0; dev->state; ) {
        if (!dev->ep->WaitForXfer(&ov[i], TO_TRANSFER)) {
            fprintf(stderr, "bulk transfer timeout\n");
            continue;
        }
        if (!dev->ep->FinishDataXfer(dev->buff[i], len, &ov[i], ctx[i])) {
            fprintf(stderr, "bulk transfer error\n");
            break;
        }
        ctx[i] = dev->ep->BeginDataXfer(dev->buff[i], len, &ov[i]);
        dev->wp = i;
        i = (i + 1) % SDR_MAX_BUFF;
    }
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        dev->ep->FinishDataXfer(dev->buff[i], len, &ov[i], ctx[i]);
        CloseHandle(ov[i].hEvent);
    }
    return 0;
}

#else /* CYUSB */

/* write ring-buffer ---------------------------------------------------------*/
static int write_buff(sdr_dev_t *dev, uint8_t *data)
{
    int wp = (dev->wp + 1) % SDR_MAX_BUFF;
    if (wp == dev->rp) {
        return 0;
    }
    dev->buff[wp] = data;
    dev->wp = wp;
    return 1;
}

/* read ring-buffer ----------------------------------------------------------*/
static uint8_t *read_buff(sdr_dev_t *dev)
{
    uint8_t *data;
    
    if (dev->rp == dev->wp) {
        return NULL;
    }
    data = dev->buff[dev->rp];
    dev->rp = (dev->rp + 1) % SDR_MAX_BUFF;
    return data;
}

/* USB bulk transfer callback ------------------------------------------------*/
static void transfer_cb(struct libusb_transfer *transfer)
{
    sdr_dev_t *dev = (sdr_dev_t *)transfer->user_data;
    
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "USB bulk transfer error (%d)\n", transfer->status);
    }
    else if (!write_buff(dev, transfer->buffer)) {
        fprintf(stderr, "USB bulk transfer buffer overflow\n");
    }
    libusb_submit_transfer(transfer);
}

/* USB event handler thread --------------------------------------------------*/
static void *event_handler_thread(void *arg)
{
    sdr_dev_t *dev = (sdr_dev_t *)arg;
    
    while (dev->state) {
        libusb_handle_events(NULL);
    }
    return NULL;
}

#endif /* CYUSB */

/*----------------------------------------------------------------------------*/
/**
 *  Open a SDR device.
 *
 *  args:
 *      bus         (I)   USB bus number of SDR device  (-1:any)
 *      port        (I)   USB port number of SDR device (-1:any)
 *
 *  return
 *      SDR device pointer (NULL: error)
 */
sdr_dev_t *sdr_dev_open(int bus, int port)
{
    int i;
#ifdef CYUSB
    sdr_dev_t *dev = new sdr_dev_t;
    
    if (!(dev->usb = sdr_usb_open(bus, port, SDR_DEV_VID, SDR_DEV_PID))) {
        delete dev;
        return NULL;
    }
    if (!(dev->ep = get_bulk_ep(dev->usb, SDR_DEV_EP))) {
        sdr_usb_close(dev->usb);
        delete dev;
        return NULL;
    }
    if (!read_sample_type(dev)) {
        sdr_usb_close(dev->usb);
        delete dev;
        fprintf(stderr, "Read sampling type error\n");
        return NULL;
    }
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        dev->buff[i] = new uint8_t[SDR_SIZE_BUFF];
    }
    dev->ep->SetXferSize(SDR_SIZE_BUFF);
    gen_LUT();
    
    dev->state = 1;
    dev->rp = dev->wp = 0;
    dev->thread = CreateThread(NULL, 0, event_handler, dev, 0, NULL);
    
    return dev;
#else /* CYUSB */
    sdr_dev_t *dev;
    struct sched_param param = {99};
    
    dev = (sdr_dev_t *)sdr_malloc(sizeof(sdr_dev_t));
    
    if (!(dev->usb = sdr_usb_open(bus, port, SDR_DEV_VID, SDR_DEV_PID))) {
        sdr_free(dev);
        return NULL;
    }
    if (!read_sample_type(dev)) {
        sdr_usb_close(dev->usb);
        sdr_free(dev);
        fprintf(stderr, "Read sampling type error\n");
        return NULL;
    }
    for (i = 0; i < SDR_MAX_BUFF; i++) {
#if 1
        dev->data[i] = libusb_dev_mem_alloc(dev->usb, SDR_SIZE_BUFF);
#else
        dev->data[i] = (uint8_t *)sdr_malloc(SDR_SIZE_BUFF);
#endif
        if (!(dev->transfer[i] = libusb_alloc_transfer(0))) {
            sdr_usb_close(dev->usb);
            sdr_free(dev);
            return NULL;
        }
        libusb_fill_bulk_transfer(dev->transfer[i], dev->usb, SDR_DEV_EP,
            dev->data[i], SDR_SIZE_BUFF, transfer_cb, dev, TO_TRANSFER);
    }
    gen_LUT();
    
    dev->state = 1;
    dev->rp = dev->wp = 0;
    pthread_create(&dev->thread, NULL, event_handler_thread, dev);
    
    /* set thread scheduling real-time */
    if (pthread_setschedparam(dev->thread, SCHED_RR, &param)) {
        fprintf(stderr, "set thread scheduling error\n");
    }
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        libusb_submit_transfer(dev->transfer[i]);
    }
    return dev;

#endif /* CYUSB */
}

/*----------------------------------------------------------------------------*/
/**
 *  Close SDR device.
 *
 *  args:
 *      dev         (I)   SDR device
 *
 *  return
 *      none
 */
void sdr_dev_close(sdr_dev_t *dev)
{
    int i;

#ifdef CYUSB
    dev->state = 0;
    WaitForSingleObject(dev->thread, 10000);
    CloseHandle(dev->thread);
    
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        delete [] dev->buff[i];
    }
    sdr_usb_close(dev->usb);
    delete dev;
#else
    dev->state = 0;
    pthread_join(dev->thread, NULL);
    
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        libusb_cancel_transfer(dev->transfer[i]);
    }
    sdr_sleep_msec(100);
    sdr_usb_close(dev->usb);
    
    for (i = 0; i < SDR_MAX_BUFF; i++) {
        libusb_free_transfer(dev->transfer[i]);
#if 1
        libusb_dev_mem_free(dev->usb, dev->data[i], SDR_SIZE_BUFF);
#else
        sdr_free(dev->data[i]);
#endif
    }
    sdr_free(dev);
#endif /* CYUSB */
}

/* copy digital IF data ------------------------------------------------------*/
static int copy_data(const uint8_t *data, int ch, int IQ, int8_t *buff)
{
    int i, j, size = SDR_SIZE_BUFF;
    
    if (IQ == 0) { /* raw */
        if (ch != 0) return 0;
        memcpy(buff, data, size);
    }
    else if (IQ == 1) { /* I sampling */
        for (i = 0; i < size; i += 4) {
            buff[i  ] = LUT[ch][0][data[i  ]];
            buff[i+1] = LUT[ch][0][data[i+1]];
            buff[i+2] = LUT[ch][0][data[i+2]];
            buff[i+3] = LUT[ch][0][data[i+3]];
        }
    }
    else if (IQ == 2) { /* I/Q sampling */
        size *= 2;
        for (i = j = 0; i < size; i += 8, j += 4) {
            buff[i  ] = LUT[ch][0][data[j  ]];
            buff[i+1] = LUT[ch][1][data[j  ]];
            buff[i+2] = LUT[ch][0][data[j+1]];
            buff[i+3] = LUT[ch][1][data[j+1]];
            buff[i+4] = LUT[ch][0][data[j+2]];
            buff[i+5] = LUT[ch][1][data[j+2]];
            buff[i+6] = LUT[ch][0][data[j+3]];
            buff[i+7] = LUT[ch][1][data[j+3]];
        }
    }
    return size;
}

/* get digital IF data -------------------------------------------------------*/
int sdr_dev_data(sdr_dev_t *dev, int8_t **buff, int *n)
{
    uint8_t *data;
    int size = 0;
    
    n[0] = n[1] = 0;
    
    while ((data = read_buff(dev))) {
        n[0] += copy_data(data, 0, dev->IQ[0], buff[0] + n[0]);
        n[1] += copy_data(data, 1, dev->IQ[1], buff[1] + n[1]);
        size += SDR_SIZE_BUFF;
    }
    return size;
}
