#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include <fcntl.h>              // Flags for open()
#include <sys/stat.h>           // Open() system call
#include <sys/types.h>          // Types for open()
#include <unistd.h>             // Close() system call
#include <string.h>             // Memory setting and copying
#include <getopt.h>             // Option parsing
#include <errno.h>              // Error codes

#include "util.h"               // Miscellaneous utilities
#include "conversion.h"         // Convert bytes to MiBs
#include "libaxidma.h"          // Interface ot the AXI DMA library

/*----------------------------------------------------------------------------
 * Internal Definitions
 *----------------------------------------------------------------------------*/

// A convenient structure to carry information around about the transfer
struct dma_transfer {
    int input_fd;           // The file descriptor for the input file
    int input_channel;      // The channel used to send the data
    int input_size;         // The amount of data to send
    void *input_buf;        // The buffer to hold the input data
    int output_fd;          // The file descriptor for the output file
    int output_channel;     // The channel used to receive the data
    int output_size;        // The amount of data to receive
    void *output_buf;       // The buffer to hold the output
};

/*----------------------------------------------------------------------------
 * Command Line Interface
 *----------------------------------------------------------------------------*/

// Prints the usage for this program
static void print_usage(bool help)
{
    FILE* stream = (help) ? stdout : stderr;

    fprintf(stream, "Usage: axidma_stdio"
            "[-t <DMA tx channel>] [-r <DMA rx channel>].\n");
    if (!help) {
        return;
    }

    fprintf(stream, "\t-t <DMA tx channel>:\tThe device id of the DMA channel "
            "to use for transmitting the file. Default is to use the lowest "
            "numbered channel available.\n");
    fprintf(stream, "\t-r <DMA rx channel>:\tThe device id of the DMA channel "
            "to use for receiving the data from the PL fabric. Default is to "
            "use the lowest numbered channel available.\n");
    return;
}

/* Parses the command line arguments overriding the default transfer sizes,
 * and number of transfer to use for the benchmark if specified. */
static int parse_args(int argc, char **argv, int *input_channel, int *output_channel, int *output_size)
{
    char option;
    int int_arg;
    double double_arg;
    bool o_specified, s_specified;
    int rc;

    // Set the default values for the arguments
    *input_channel = -1;
    *output_channel = -1;
    *output_size = -1;
    o_specified = false;
    s_specified = false;
    rc = 0;

    while ((option = getopt(argc, argv, "t:r:h")) != (char)-1)
    {
        switch (option)
        {
            // Parse the transmit channel device id
            case 't':
                rc = parse_int(option, optarg, &int_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *input_channel = int_arg;
                break;

            // Parse the receive channel device id
            case 'r':
                rc = parse_int(option, optarg, &int_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *output_channel = int_arg;
                break;

            case 'h':
                print_usage(true);
                exit(0);

            default:
                print_usage(false);
                return -EINVAL;
        }
    }

    // If one of -t or -r is specified, then both must be
    if ((*input_channel == -1) ^ (*output_channel == -1)) {
        fprintf(stderr, "Error: Either both -t and -r must be specified, or "
                "neither.\n");
        print_usage(false);
        return -EINVAL;
    }

    return 0;
}

/*----------------------------------------------------------------------------
 * DMA File Transfer Functions
 *----------------------------------------------------------------------------*/
static int transfer_from_stdio(axidma_dev_t dev, struct dma_transfer *trans)
{
    //本函数先循环从标准输入中读取单精度浮点数，然后将读取到的数据写入到DMA的输入通道中
    //然后从DMA的输出通道中读取数据，将数据写入到标准输出中
    #define MAX_FLOATS 1000
    int rc;
    float input;
    float output;
    char *bytes = (char *)malloc(MAX_FLOATS * sizeof(float));
    
    size_t size = 0;
    int result;

    if (bytes == NULL) {
        fprintf(stderr, "Failed to allocate memory.\n");
        return -1;
    }

    while (1) {
        result = scanf("%f", &input);
        if (result == EOF || result == 0) {
            break;
        }
        printf("Read float: %f\n", input);
        if (size >= MAX_FLOATS * sizeof(float)) {
            fprintf(stderr, "Exceeded maximum number of floats.\n");
            break;
        }
        memcpy(bytes + size, &input, sizeof(float));
        size += sizeof(float);
    }

    trans->input_size = size;
    trans->output_size = size;
    trans->input_buf = axidma_malloc(dev, trans->input_size);
    if (trans->input_buf == NULL) {
        fprintf(stderr, "Failed to allocate the input buffer.\n");
        rc = -ENOMEM;
        goto ret;
    }
    trans->output_buf = axidma_malloc(dev, trans->output_size);
    if (trans->output_buf == NULL) {
        rc = -ENOMEM;
        goto free_input_buf;
    }

    memcpy(trans->input_buf, bytes, trans->input_size);
    for (size_t i = 0; i < size; i++) {
        printf("Input Byte %zu: %02x\n", i, bytes[i] & 0xff);
    }

    rc = axidma_twoway_transfer(dev, trans->input_channel, trans->input_buf,
            trans->input_size, NULL, trans->output_channel, trans->output_buf,
            trans->output_size, NULL, true);
    if (rc < 0) {
        fprintf(stderr, "DMA read write transaction failed.\n");
        goto free_output_buf;
    }
    char *output_bytes = (char *)malloc(trans->output_size * sizeof(float));
    memcpy(output_bytes, trans->output_buf, trans->output_size);
    for (size_t i = 0; i < size; i++) {
        printf("Output Byte %zu: %02x\n", i, output_bytes[i] & 0xff);
    }
    for (size_t i = 0; i < size; i += sizeof(float)) {
        memcpy(&output, output_bytes + i, sizeof(float));
        printf("Output float: %f\n", output);
    }
    
free_output_buf:
    axidma_free(dev, trans->output_buf, trans->output_size);
free_input_buf:
    axidma_free(dev, trans->input_buf, trans->input_size);
ret:
    return rc;
}
static int transfer_file(axidma_dev_t dev, struct dma_transfer *trans,
                         char *output_path)
{
    int rc;

    // Allocate a buffer for the input file, and read it into the buffer
    trans->input_buf = axidma_malloc(dev, trans->input_size);
    if (trans->input_buf == NULL) {
        fprintf(stderr, "Failed to allocate the input buffer.\n");
        rc = -ENOMEM;
        goto ret;
    }
    rc = robust_read(trans->input_fd, trans->input_buf, trans->input_size);
    if (rc < 0) {
        perror("Unable to read in input buffer.\n");
        axidma_free(dev, trans->input_buf, trans->input_size);
        return rc;
    }

    // Allocate a buffer for the output file
    trans->output_buf = axidma_malloc(dev, trans->output_size);
    if (trans->output_buf == NULL) {
        rc = -ENOMEM;
        goto free_input_buf;
    }

    // Perform the transfer
    // Perform the main transaction
    rc = axidma_twoway_transfer(dev, trans->input_channel, trans->input_buf,
            trans->input_size, NULL, trans->output_channel, trans->output_buf,
            trans->output_size, NULL, true);
    if (rc < 0) {
        fprintf(stderr, "DMA read write transaction failed.\n");
        goto free_output_buf;
    }

    // Write the data to the output file
    printf("Writing output data to `%s`.\n", output_path);
    rc = robust_write(trans->output_fd, trans->output_buf, trans->output_size);

free_output_buf:
    axidma_free(dev, trans->output_buf, trans->output_size);
free_input_buf:
    axidma_free(dev, trans->input_buf, trans->input_size);
ret:
    return rc;
}

/*----------------------------------------------------------------------------
 * Main
 *----------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    int rc;
    char *input_path, *output_path;
    axidma_dev_t axidma_dev;
    struct stat input_stat;
    struct dma_transfer trans;
    const array_t *tx_chans, *rx_chans;

    // Parse the input arguments
    memset(&trans, 0, sizeof(trans));
    if (parse_args(argc, argv, &trans.input_channel,
                   &trans.output_channel, &trans.output_size) < 0) {
        rc = 1;
        goto ret;
    }

    // Initialize the AXIDMA device
    axidma_dev = axidma_init();
    if (axidma_dev == NULL) {
        fprintf(stderr, "Error: Failed to initialize the AXI DMA device.\n");
        rc = 1;
        goto close_output;
    }

    // Get the size of the input file
    if (fstat(trans.input_fd, &input_stat) < 0) {
        perror("Unable to get file statistics");
        rc = 1;
        goto destroy_axidma;
    }

    // If the output size was not specified by the user, set it to the default
    trans.input_size = input_stat.st_size;
    if (trans.output_size == -1) {
        trans.output_size = trans.input_size;
    }

    // Get the tx and rx channels if they're not already specified
    tx_chans = axidma_get_dma_tx(axidma_dev);
    if (tx_chans->len < 1) {
        fprintf(stderr, "Error: No transmit channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }
    rx_chans = axidma_get_dma_rx(axidma_dev);
    if (rx_chans->len < 1) {
        fprintf(stderr, "Error: No receive channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }

    /* If the user didn't specify the channels, we assume that the transmit and
     * receive channels are the lowest numbered ones. */
    if (trans.input_channel == -1 && trans.output_channel == -1) {
        trans.input_channel = tx_chans->data[0];
        trans.output_channel = rx_chans->data[0];
    }

    rc = transfer_from_stdio(axidma_dev, &trans);
    printf("AXI DMA File Transfer Info:\n");
    printf("\tTransmit Channel: %d\n", trans.input_channel);
    printf("\tReceive Channel: %d\n", trans.output_channel);
    printf("\tInput File Size: %.2f MiB\n", BYTE_TO_MIB(trans.input_size));
    printf("\tOutput File Size: %.2f MiB\n\n", BYTE_TO_MIB(trans.output_size));

    // Transfer the file over the AXI DMA
    //rc = transfer_file(axidma_dev, &trans, output_path);
    rc = (rc < 0) ? -rc : 0;

destroy_axidma:
    axidma_destroy(axidma_dev);
ret:
    return rc;
}
