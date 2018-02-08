#include <iostream>
#include "allreduce.hpp"
#include "test_utils.hpp"

#include <stdlib.h>
#include <math.h>
#include "common.h"


#define NCCL_THRESHOLD	1e-05

void test_nccl_allreduce(const std::vector<float>& expected,
                         std::vector<float> input,
                         allreduces::NCCLCommunicator& nccl_comm);

const size_t max_size = 1<<30;
const float eps = 1e-4;

bool check_vector(const std::vector<float>& expected,
                  const std::vector<float>& actual) {
  for (size_t i = 0; i < expected.size(); ++i) {
    if (std::abs(expected[i] - actual[i]) > eps) {
      return false;
    }
  }
  return true;
}

/**
 * Test allreduce algo on input, check with expected.
 */
template <typename Backend>
void test_allreduce_algo(const std::vector<float>& expected,
                         std::vector<float> input,
                         typename Backend::comm_type& comm,
                         typename Backend::algo_type algo) {
  std::vector<float> recv(input.size());
  // Test regular allreduce.
  allreduces::Allreduce<float, Backend>(input.data(), recv.data(), input.size(),
                                        allreduces::ReductionOperator::sum, comm, algo);
  if (!check_vector(expected, recv)) {
    std::cout << comm.rank() << ": regular allreduce does not match" <<
      std::endl;
  }
  // Test in-place allreduce.
  allreduces::Allreduce<float, Backend>(input.data(), input.size(),
                                        allreduces::ReductionOperator::sum, comm, algo);
  if (!check_vector(expected, input)) {
    std::cout << comm.rank() << ": in-place allreduce does not match" <<
      std::endl;
  }
}

/**
 * Test non-blocking allreduce algo on input, check with expected.
 */
void test_nb_allreduce_algo(const std::vector<float>& expected,
                            std::vector<float> input,
                            allreduces::Communicator& comm,
                            allreduces::AllreduceAlgorithm algo) {
  allreduces::AllreduceRequest req;
  std::vector<float> recv(input.size());
  // Test regular allreduce.
  allreduces::NonblockingAllreduce(input.data(), recv.data(), input.size(),
                                   allreduces::ReductionOperator::sum, comm,
                                   req, algo);
  allreduces::Wait(req);
  if (!check_vector(expected, recv)) {
    std::cout << comm.rank() << ": regular allreduce does not match" <<
      std::endl;
  }
  // Test in-place allreduce.
  allreduces::NonblockingAllreduce(input.data(), input.size(),
                                   allreduces::ReductionOperator::sum, comm,
                                   req, algo);
  allreduces::Wait(req);
  if (!check_vector(expected, input)) {
    std::cout << comm.rank() << ": in-place allreduce does not match" <<
      std::endl;
  }
}

int main(int argc, char** argv) {
  allreduces::Initialize(argc, argv);

  int code = 0;
  if(argc == 1){
    code = 0;
  }
  else if(argc == 2) {
    code = atoi(argv[1]);
    if(code != 0 && code != 1){
      std::cerr << "usage: " << argv[0] << " [0(MPI) | 1(NCCL)]\n";
      return -1;
    }
  }
  else{
    std::cerr << "usage: " << argv[0] << " [0(MPI) | 1(NCCL)]\n";
    return -1;
  }

  if(code == 0){
    // Add algorithms to test here.
    std::vector<allreduces::AllreduceAlgorithm> algos = {
      allreduces::AllreduceAlgorithm::automatic,
      allreduces::AllreduceAlgorithm::mpi_passthrough,
      allreduces::AllreduceAlgorithm::mpi_recursive_doubling,
      allreduces::AllreduceAlgorithm::mpi_ring,
      allreduces::AllreduceAlgorithm::mpi_rabenseifner,
      allreduces::AllreduceAlgorithm::mpi_pe_ring
    };
    std::vector<allreduces::AllreduceAlgorithm> nb_algos = {
      allreduces::AllreduceAlgorithm::automatic,
      allreduces::AllreduceAlgorithm::mpi_passthrough,
      allreduces::AllreduceAlgorithm::mpi_recursive_doubling,
      allreduces::AllreduceAlgorithm::mpi_ring,
      allreduces::AllreduceAlgorithm::mpi_rabenseifner,
      //allreduces::AllreduceAlgorithm::mpi_pe_ring
    };
    allreduces::MPICommunicator comm;  // Use COMM_WORLD.
    // Compute sizes to test.
    std::vector<size_t> sizes = {0};
    for (size_t size = 1; size <= max_size; size *= 2) {
      sizes.push_back(size);
      // Avoid duplicating 2.
      if (size > 1) {
        sizes.push_back(size + 1);
      }
    }
    for (const auto& size : sizes) {
      if (comm.rank() == 0) {
        std::cout << "Testing size " << human_readable_size(size) << std::endl;
      }
      // Compute true value.
      std::vector<float> data = gen_data(size);
      std::vector<float> expected(data);
      MPI_Allreduce(MPI_IN_PLACE, expected.data(), size, MPI_FLOAT, MPI_SUM,
                    MPI_COMM_WORLD);
      // Test algorithms.
      for (auto&& algo : algos) {
        MPI_Barrier(MPI_COMM_WORLD);
        if (comm.rank() == 0) {
          std::cout << " Algo: " << allreduces::allreduce_name(algo) << std::endl;
        }
        test_allreduce_algo<allreduces::MPIBackend>(expected, data, comm, algo);
      }
      for (auto&& algo : nb_algos) {
        MPI_Barrier(MPI_COMM_WORLD);
        if (comm.rank() == 0) {
          std::cout << " Algo: NB " << allreduces::allreduce_name(algo) << std::endl;
        }
        test_nb_allreduce_algo(expected, data, comm, algo);
      }
    }
  }
  else{
    allreduces::NCCLCommunicator nccl_comm; 
    // Compute sizes to test.
    std::vector<size_t> sizes = {0};
    for (size_t size = 1; size <= max_size; size *= 2) {
      sizes.push_back(size);
      // Avoid duplicating 2.
      if (size > 1) {
        sizes.push_back(size + 1);
      }
    }

    for (const auto& size : sizes) {

      if (nccl_comm.rank() == 0) {
        std::cout << "Testing size " << human_readable_size(size) << std::endl;
      }
      // Compute true value.
      std::vector<float> data = gen_data(size);
      std::vector<float> expected(data);
      MPI_Allreduce(MPI_IN_PLACE, expected.data(), size, MPI_FLOAT, MPI_SUM,
                    MPI_COMM_WORLD);
        
      MPI_Barrier(MPI_COMM_WORLD);
      test_nccl_allreduce(expected, data, nccl_comm);
    }
  }
    
  allreduces::Finalize();
  return 0;
}




///================
/**
 * Test NCCL allreduce algo on input, check with expected.
 */
void test_nccl_allreduce(const std::vector<float>& expected,
                         std::vector<float> input,
                         allreduces::NCCLCommunicator& nccl_comm) {
  /// create and copy input to device memory
  /// create a receive buffer in device
  
  float *sbuffer;
  float *rbuffer;
  size_t len = input.size() * sizeof(float);

  CUDACHECK(cudaMalloc((void **)&sbuffer, len));
  CUDACHECK(cudaMalloc((void **)&rbuffer, len));
  CUDACHECK(cudaMemcpy(sbuffer, input.data(), len, cudaMemcpyHostToDevice));


  allreduces::Allreduce(sbuffer, rbuffer, input.size(), allreduces::ReductionOperator::sum, nccl_comm);
  //allreduces::NCCLAllreduce(sbuffer, rbuffer, input.size(), allreduces::ReductionOperator::sum, nccl_comm);

  std::vector<float> recv(input.size());
  CUDACHECK(cudaMemcpy(&recv[0], rbuffer, len, cudaMemcpyDeviceToHost));


  /// Since some numerical errors are expected when running on GPU, 
  /// we need to measure the error between the two

  float sum_exp = 0.0;
  float sum_recv = 0.0;

  for(size_t i=0; i<input.size(); i++){
    sum_exp += expected[i];
    sum_recv += recv[i];
  }

  int myid = nccl_comm.rank();
  if(myid == 0) {
      
    //std::cout << "sum_exp= " << sum_exp << " sum_recv= " << sum_recv << std::endl;
    if(fabsf(sum_exp-sum_recv > NCCL_THRESHOLD)){
      std::cout << ": NCCL allreduce does not match" << std::endl;
    }
  }
  CUDACHECK(cudaFree(sbuffer));
  CUDACHECK(cudaFree(rbuffer));
}
