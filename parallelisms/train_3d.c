// 3d Parallel training loop.
// 
// This is the culmination of our efforts which composes all implemented
// parallelisms. 
//
// As of late 2024, this is the state of the art in sharding dense (i.e. non-MoE),
// short-sequence (i.e. <32k) models and allows scaling to 400B+ parameters. For 
// example, Llama 3 405B [1] was pretrained using 3d parallelism. 
// 
// To run:
//     mpicc -Ofast parallelisms/train_fsdp.c &&
//     mpirun -n <num-ranks> --map-by=:oversubscribe a.out --tp=<tp-ranks> --dp=<dp-ranks>
//
// [1]: https://arxiv.org/pdf/2407.21783


#include <mpi.h>
#include <stdlib.h>
#include <string.h>
#include "data.c"
#include "distributed.c"
#include "model.c"
#include <unistd.h>


void Model_shard_3d(Model* self, Dist* dist) {
    Model_shard_tp(self, dist->tp_rank, dist->tp_size);
    Model_shard_fsdp(self, dist->dp_rank, dist->dp_size);
    Model_shard_pp(self, dist->pp_rank);
}


float Model_forward_3d(Model* self, int* Xs, int* Ys, float* flat_buffer, Dist* dist) {
    float loss;
    if (dist->pp_rank == 0) {
        // wte forward.
        int wte_shard_size = Embedding_numel(self->wte);
        allgather(self->wte->embedding, wte_shard_size, flat_buffer, dist->dp_comm);
        float* wte_shard = self->wte->embedding;
        int wte_shard_vocab_size = self->wte->vocab_size;
        self->wte->embedding = flat_buffer;
        self->wte->vocab_size = wte_shard_vocab_size * dist->dp_size;
        Embedding_forward(self->wte, Xs, self->wte_out);
        self->wte->embedding = wte_shard;
        self->wte->vocab_size = wte_shard_vocab_size; 

        send(self->wte_out->value, Activation_numel(self->wte_out), /* to_rank */ 1, dist->pp_comm);
    } else if (dist->pp_rank == 1) {
        recv(self->wte_out_flat->value, Activation_numel(self->wte_out_flat), /* from_rank */ 0, dist->pp_comm);

        // fc_1 forward.
        int fc_1_shard_size = Linear_weight_numel(self->fc_1);
        allgather(self->fc_1->weight, fc_1_shard_size, flat_buffer, dist->dp_comm);
        float* fc_1_shard = self->fc_1->weight;
        int fc_1_shard_in_features = self->fc_1->in_features;
        self->fc_1->weight = flat_buffer;
        self->fc_1->in_features = fc_1_shard_in_features * dist->dp_size;
        Linear_forward(self->fc_1, self->wte_out_flat, self->fc_1_out);
        self->fc_1->weight = fc_1_shard;
        self->fc_1->in_features = fc_1_shard_in_features;

        relu(self->fc_1_out, self->relu_out);
        send(self->relu_out->value, Activation_numel(self->relu_out), /* to_rank */ 2, dist->pp_comm);
    } else if (dist->pp_rank == 2) {
        recv(self->relu_out->value, Activation_numel(self->relu_out), /* from_rank */ 1, dist->pp_comm);

        // fc_2 forward.
        int fc_2_shard_size = Linear_weight_numel(self->fc_2);
        allgather(self->fc_2->weight, fc_2_shard_size, flat_buffer, dist->dp_comm);
        float* fc_2_shard = self->fc_2->weight;
        int fc_2_shard_in_features = self->fc_2->in_features;
        self->fc_2->weight = flat_buffer;
        self->fc_2->in_features = fc_2_shard_in_features * dist->dp_size;
        Linear_forward(self->fc_2, self->relu_out, self->fc_2_out);
        self->fc_2->weight = fc_2_shard;
        self->fc_2->in_features = fc_2_shard_in_features;

        allreduce_mean(self->fc_2_out->value, Activation_numel(self->fc_2_out), dist->tp_comm, dist->tp_size);
        softmax(self->fc_2_out, self->softmax_out);
        loss = cross_entropy_loss(self->softmax_out, Ys);
    } else {
        printf("Unknown pp_rank: %d\n", dist->pp_rank);
        MPI_Finalize();
        exit(1);
    }
    // We don't technically need to broadcast here, but it's nicer if all the ranks have the
    // same loss value at the end.
    MPI_Bcast(&loss, /* count */ 1, MPI_FLOAT, /* root */ 2, dist->pp_comm);
    return loss;
}

 
int main(int argc, char** argv) {
    int global_batch_size = 32;
    int seq_len = 16;  // seq_len is computed offline and is equal to the longest word.
    int vocab_size = 27;
    int emb_size = 16;
    int hidden_size = 4 * emb_size;

    // Initialize environment. 
    int tp_size, dp_size; 
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--tp") == 0) {
            tp_size = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--dp") == 0){
            dp_size = atoi(argv[i+1]);
        }
    }
    int pp_size = 3;  // Pipeline parallelism only supports 3 ranks.
    srand(42);
    MPI_Init(&argc, &argv);
    Dist* dist = Dist_create(tp_size, dp_size, pp_size);

    // Compute per-rank batch size from the global batch size.
    if (global_batch_size % dist->dp_size != 0) {
        rank0_printf(dist->world_rank, "Global batch size must be divisible by world size!\n");
        MPI_Finalize();
        exit(1);
    }
    int batch_size = global_batch_size / dist->dp_size;
    rank0_printf(dist->world_rank, "Micro batch_size: %d\n", batch_size); 

    // Create dataset.
    Dataset* dataset = Dataset_create_from_file("data/names.txt", seq_len);
    Dataset train_split, test_split;
    Dataset_train_test_split(dataset, &train_split, &test_split, /* train_percent */ 0.9);
    int* global_Xs = malloc(sizeof(int) * global_batch_size * seq_len);
    int* global_Ys = malloc(sizeof(int) * global_batch_size);
    int* Xs = malloc(sizeof(int) * batch_size * seq_len);
    int* Ys = malloc(sizeof(int) * batch_size);

    // Create model with padded vocab.
    // Hack! We first construct the full model then shard the parameters. This is just to 
    // ensure that the model parameters are initialized in the exact same way as the single-threaded
    // training loop for easy comparision. In practice, this approach would OOM for large models.
    Model* model = Model_create(batch_size, seq_len, vocab_size, emb_size, hidden_size);
    // Hack! We manually construct the padded embedding instead of using vocab_size_padded in
    // Model_create above. This ensures that the RNG state matches the single-threaded training
    // loop for easy comparison.
    int vocab_size_padded = vocab_size + (dist->dp_size - (vocab_size % dist->dp_size));
    Model_pad_vocab(model, vocab_size_padded);
    rank0_printf(dist->world_rank, "Padded vocab size: %d\n", vocab_size_padded);
    // Create temporary buffer to store allgathered params/grads of individual layers.
    int max_layer_size = 0;
    max_layer_size = max(Embedding_numel(model->wte) * dist->dp_size, max_layer_size);
    max_layer_size = max(Linear_weight_numel(model->fc_1) * dist->dp_size, max_layer_size);
    max_layer_size = max(Linear_weight_numel(model->fc_2) * dist->dp_size, max_layer_size);
    rank0_printf(dist->world_rank, "Maximum layer size: %d\n", max_layer_size);
    float* flat_buffer = malloc(sizeof(float) * 2 * max_layer_size);  // Account for gradients.
    // Shard the model. Must happen _after_ the temporary buffer creation because Model_shard_pp
    // deallocates fc_1 and fc_2.
    Model_shard_3d(model, dist);


    // Train.
    Dataset_get_rank_batch(
        &train_split, global_Xs, global_Ys, Xs, Ys, global_batch_size, dist->dp_rank, dist->dp_size 
    );
    float loss = Model_forward_3d(model, Xs, Ys, flat_buffer, dist);
    allreduce_mean(&loss, /* size */ 1, dist->dp_comm, dist->dp_size);
    rank0_printf(dist->world_rank, "step: %d, loss %f\n", 0, loss);
 
    MPI_Finalize();
    return 0;
}