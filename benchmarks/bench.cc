#include <iostream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>

#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>

#include "bench.h"
#include "bdb_wrapper.h"
#include "ndb_wrapper.h"
#include "mysql_wrapper.h"

using namespace std;
using namespace util;

size_t nthreads = 1;
volatile bool running = true;
int verbose = 0;
uint64_t txn_flags = 0;
double scale_factor = 1.0;
uint64_t runtime = 30;

template <typename T>
static void
delete_pointers(const vector<T *> &pts)
{
  for (size_t i = 0; i < pts.size(); i++)
    delete pts[i];
}

template <typename T>
static vector<T>
elemwise_sum(const vector<T> &a, const vector<T> &b)
{
  INVARIANT(a.size() == b.size());
  vector<T> ret(a.size());
  for (size_t i = 0; i < a.size(); i++)
    ret[i] = a[i] + b[i];
  return ret;
}

void
bench_runner::run()
{
  // load data
  const vector<bench_loader *> loaders = make_loaders();
  {
    scoped_timer t("dataloading", verbose);
    for (vector<bench_loader *>::const_iterator it = loaders.begin();
         it != loaders.end(); ++it)
      (*it)->start();
    for (vector<bench_loader *>::const_iterator it = loaders.begin();
         it != loaders.end(); ++it)
      (*it)->join();
  }

  db->do_txn_epoch_sync();

  if (verbose) {
    for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
         it != open_tables.end(); ++it)
      cerr << "table " << it->first << " size " << it->second->size() << endl;
    cerr << "starting benchmark..." << endl;
  }

  const vector<bench_worker *> workers = make_workers();
  ALWAYS_ASSERT(!workers.empty());
  for (vector<bench_worker *>::const_iterator it = workers.begin();
       it != workers.end(); ++it)
    (*it)->start();

  barrier_a.wait_for(); // wait for all threads to start up
  barrier_b.count_down(); // bombs away!
  timer t;
  sleep(runtime);
  running = false;
  __sync_synchronize();
  const unsigned long elapsed = t.lap();
  size_t n_commits = 0;
  size_t n_aborts = 0;
  for (size_t i = 0; i < nthreads; i++) {
    workers[i]->join();
    n_commits += workers[i]->get_ntxn_commits();
    n_aborts += workers[i]->get_ntxn_aborts();
  }

  const double agg_throughput = double(n_commits) / (double(elapsed) / 1000000.0);
  const double avg_per_core_throughput = agg_throughput / double(workers.size());

  const double agg_abort_rate = double(n_aborts) / (double(elapsed) / 1000000.0);
  const double avg_per_core_abort_rate = agg_abort_rate / double(workers.size());

  if (verbose) {
    vector<size_t> agg_txn_counts = workers[0]->get_txn_counts();
    for (size_t i = 1; i < workers.size(); i++)
      agg_txn_counts = elemwise_sum(agg_txn_counts, workers[i]->get_txn_counts());
    cerr << "agg_throughput: " << agg_throughput << " ops/sec" << endl;
    cerr << "avg_per_core_throughput: " << avg_per_core_throughput << " ops/sec/core" << endl;
    cerr << "agg_abort_rate: " << agg_abort_rate << " aborts/sec" << endl;
    cerr << "avg_per_core_abort_rate: " << avg_per_core_abort_rate << " aborts/sec/core" << endl;
    cerr << "txn breakdown: " << format_list(agg_txn_counts.begin(), agg_txn_counts.end()) << endl;
  }

  // output for plotting script
  cout << agg_throughput << " " << agg_abort_rate << endl;

  db->do_txn_finish();

  delete_pointers(loaders);
  delete_pointers(workers);
}

int
main(int argc, char **argv)
{
  abstract_db *db = NULL;
  void (*test_fn)(abstract_db *) = NULL;
  string bench_type = "ycsb";
  string db_type = "ndb-proto2";
  char *curdir = get_current_dir_name();
  string basedir = curdir;
  free(curdir);
  while (1) {
    static struct option long_options[] =
    {
      {"verbose",      no_argument,       &verbose, 1},
      {"bench",        required_argument, 0,       'b'},
      {"scale-factor", required_argument, 0,       's'},
      {"num-threads",  required_argument, 0,       't'},
      {"db-type",      required_argument, 0,       'd'},
      {"basedir",      required_argument, 0,       'B'},
      {"txn-flags",    required_argument, 0,       'f'},
      {"runtime",      required_argument, 0,       'r'},
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "vb:s:t:d:B:f:r:", long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;
      abort();
      break;

    case 'b':
      bench_type = optarg;
      break;

    case 's':
      scale_factor = strtod(optarg, NULL);
      ALWAYS_ASSERT(scale_factor > 0.0);
      break;

    case 't':
      nthreads = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(nthreads > 0);
      break;

    case 'd':
      db_type = optarg;
      break;

    case 'B':
      basedir = optarg;
      break;

    case 'f':
      txn_flags = strtoul(optarg, NULL, 10);
      break;

    case 'r':
      runtime = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(runtime > 0);
      break;

    case '?':
      /* getopt_long already printed an error message. */
      break;

    default:
      abort();
    }
  }

  if (bench_type == "ycsb")
    test_fn = ycsb_do_test;
  else if (bench_type == "tpcc")
    test_fn = tpcc_do_test;
  else
    ALWAYS_ASSERT(false);

  if (db_type == "bdb") {
    string cmd = "rm -rf " + basedir + "/db/*";
    // XXX(stephentu): laziness
    int ret UNUSED = system(cmd.c_str());
    db = new bdb_wrapper("db", bench_type + ".db");
  } else if (db_type == "ndb-proto1") {
    db = new ndb_wrapper(ndb_wrapper::PROTO_1);
  } else if (db_type == "ndb-proto2") {
    db = new ndb_wrapper(ndb_wrapper::PROTO_2);
  } else if (db_type == "mysql") {
    string dbdir = basedir + "/mysql-db";
    db = new mysql_wrapper(dbdir, bench_type);
  } else
    ALWAYS_ASSERT(false);

#ifdef CHECK_INVARIANTS
  cerr << "WARNING: invariant checking is enabled - should disable for benchmark" << endl;
#endif

  if (verbose) {
    cerr << "settings:"                             << endl;
    cerr << "  bench       : " << bench_type        << endl;
    cerr << "  scale       : " << scale_factor      << endl;
    cerr << "  num-threads : " << nthreads          << endl;
    cerr << "  db-type     : " << db_type           << endl;
    cerr << "  basedir     : " << basedir           << endl;
    cerr << "  txn-flags   : " << hexify(txn_flags) << endl;
    cerr << "  runtime     : " << runtime           << endl;
  }

  test_fn(db);

  delete db;
  return 0;
}
