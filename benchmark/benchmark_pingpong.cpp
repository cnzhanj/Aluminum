#include <iostream>
#include "Al.hpp"
#include "test_utils.hpp"
#ifdef AL_HAS_MPI_CUDA
#include "test_utils_cuda.hpp"
#include "test_utils_mpi_cuda.hpp"
#include "wait.hpp"
#endif

size_t start_size = 1;
size_t max_size = 1<<18;
//size_t start_size = 256;
//size_t max_size = 256;
size_t num_trials = 10000;

#ifdef AL_HAS_MPI_CUDA

void test_correctness() {
  cudaStream_t stream;
  cudaStreamCreate(&stream);
  typename Al::MPICUDABackend::comm_type comm(MPI_COMM_WORLD, stream);
  for (size_t size = start_size; size <= max_size; size *= 2) {
    if (comm.rank() == 0) std::cout << "Testing size " << human_readable_size(size) << std::endl;
    std::vector<float> host_data(size, 1);
    CUDAVector<float> data(host_data);
    MPI_Barrier(MPI_COMM_WORLD);
    if (comm.rank() == 0) {
      Al::Send<Al::MPICUDABackend>(data.data(), data.size(), 1, comm);
    } else if (comm.rank() == 1) {
      Al::Recv<Al::MPICUDABackend>(data.data(), data.size(), 0, comm);
    }
    cudaStreamSynchronize(stream);
    if (comm.rank() == 1) {
      std::vector<float> expected_host_data(size, 1);
      CUDAVector<float> expected(expected_host_data);
      check_vector(expected, data);
    }
  }
  cudaStreamDestroy(stream);
}

void do_benchmark() {
  cudaStream_t stream;
  cudaStreamCreate(&stream);
  typename Al::MPICUDABackend::comm_type comm(MPI_COMM_WORLD, stream);
  for (size_t size = start_size; size <= max_size; size *= 2) {
    if (comm.rank() == 0) std::cout << "Benchmarking size " << human_readable_size(size) << std::endl;
    std::vector<double> times, host_times;
    std::vector<float> host_sendbuf(size, comm.rank());
    std::vector<float> host_recvbuf(size, 0);
    CUDAVector<float> sendbuf(host_sendbuf);
    CUDAVector<float> recvbuf(host_recvbuf);
    MPI_Barrier(MPI_COMM_WORLD);
    for (size_t trial = 0; trial < num_trials; ++trial) {
      // Launch a dummy kernel just to match what the GPU version does.
      gpu_wait(0.001, stream);
      MPI_Request req1, req2;
      int flag1 = 0;
      int flag2 = 0;
      start_timer<Al::MPIBackend>(comm);
      // Using an Isend/Irecv style to better match what the GPU version uses.
      if (comm.rank() == 0) {
        //MPI_Send(host_sendbuf.data(), size, MPI_FLOAT, 1, 1, MPI_COMM_WORLD);
        //MPI_Recv(host_recvbuf.data(), size, MPI_FLOAT, 1, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Isend(host_sendbuf.data(), size, MPI_FLOAT, 1, 1, MPI_COMM_WORLD, &req1);
        MPI_Irecv(host_recvbuf.data(), size, MPI_FLOAT, 1, 1, MPI_COMM_WORLD, &req2);
        while (!flag1 || !flag2) {
          MPI_Test(&req1, &flag1, MPI_STATUS_IGNORE);
          MPI_Test(&req2, &flag2, MPI_STATUS_IGNORE);
        }
      } else if (comm.rank() == 1) {
        //MPI_Recv(host_recvbuf.data(), size, MPI_FLOAT, 0, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        //MPI_Send(host_sendbuf.data(), size, MPI_FLOAT, 0, 1, MPI_COMM_WORLD);
        MPI_Irecv(host_recvbuf.data(), size, MPI_FLOAT, 0, 1, MPI_COMM_WORLD, &req1);
        MPI_Isend(host_sendbuf.data(), size, MPI_FLOAT, 0, 1, MPI_COMM_WORLD, &req2);
        while (!flag1 || !flag2) {
          MPI_Test(&req1, &flag1, MPI_STATUS_IGNORE);
          MPI_Test(&req2, &flag2, MPI_STATUS_IGNORE);
        }
      }
      host_times.push_back(finish_timer<Al::MPIBackend>(comm) / 2);
      cudaStreamSynchronize(stream);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    for (size_t trial = 0; trial < num_trials; ++trial) {
      gpu_wait(0.001, stream);
      start_timer<Al::MPICUDABackend>(comm);
      if (comm.rank() == 0) {
        Al::Send<Al::MPICUDABackend>(sendbuf.data(), size, 1, comm);
        Al::Recv<Al::MPICUDABackend>(recvbuf.data(), size, 1, comm);
      } else if (comm.rank() == 1) {
        Al::Recv<Al::MPICUDABackend>(recvbuf.data(), size, 0, comm);
        Al::Send<Al::MPICUDABackend>(sendbuf.data(), size, 0, comm);
      }
      times.push_back(finish_timer<Al::MPICUDABackend>(comm) / 2);
    }
    times.erase(times.begin());
    host_times.erase(host_times.begin());
    if (comm.rank() == 0) {
      std::cout << "Rank 0:" << std::endl;
      std::cout << "host ";
      print_stats(host_times);
      std::cout << "mpicuda ";
      print_stats(times);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (comm.rank() == 1) {
      std::cout << "Rank 1:" << std::endl;
      std::cout << "host ";
      print_stats(host_times);
      std::cout << "mpicuda ";
      print_stats(times);
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }
  cudaStreamDestroy(stream);
}

#endif  // AL_HAS_MPI_CUDA

int main(int argc, char** argv) {
#ifdef AL_HAS_MPI_CUDA
  set_device();
  Al::Initialize(argc, argv);
  test_correctness();
  do_benchmark();
  Al::Finalize();
#else
  std::cout << "MPI-CUDA support required" << std::endl;
#endif
  return 0;
}
