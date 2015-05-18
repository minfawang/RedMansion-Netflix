#include "../includes.hpp"
#include <armadillo>
#include <iomanip>
#include <omp.h>
#include <unordered_map>
#include <tuple>
#include <mutex>

const int MAX_DATE = 2243;
const double PI = 3.141592653589793238463;
const int N_THREADS = 8;

#define _TEST_NAN
#define _USE_Y 0

#ifndef __MF_ESTIMATORS
#define __MF_ESTIMATORS

using namespace arma;

class function_table_generator {
private:
	double abspwr(int t, float beta) const{
		int sign = (t > 0) - (t < 0);
		return sign * pow(abs(1.0 * t / MAX_DATE), beta);
	}
	double sinw(int t, float w) const {
		return sin(1.0 * t / MAX_DATE * PI * w);
	}
	double cosw(int t, float w) const {
		return cos(1.0 * t / MAX_DATE * PI * w);
	}
public:
	rowvec const_table() const {
		rowvec result(2 * MAX_DATE + 1);
		result.fill(fill::ones);
		return result;
	}

	rowvec abspwr_table(float beta) const {
		rowvec result(2 * MAX_DATE + 1);
		for (int i = -MAX_DATE; i <= MAX_DATE; i++) {
			result[i + MAX_DATE] = abspwr(i, beta);
		}
		return result;
	}

	rowvec sinw_table(float w) const {
		rowvec result(2 * MAX_DATE + 1);
		for (int i = -MAX_DATE; i <= MAX_DATE; i++) {
			result[i + MAX_DATE] = sinw(i, w);
		}
		return result;
	}

	rowvec cosw_table(float w) const {
		rowvec result(2 * MAX_DATE + 1);
		for (int i = -MAX_DATE; i <= MAX_DATE; i++) {
			result[i + MAX_DATE] = cosw(i, w);
		}
		return result;
	}
};

double eval_function(const mat &function_table, int t) {
	return function_table[t + MAX_DATE];
}

vec eval_functions(const mat &function_table, int t) {
	return function_table.unsafe_col(t + MAX_DATE);
}

unsigned int get_timebin(const unsigned int date, const unsigned int D) {
	return (date - 1) * D / MAX_DATE;
}

class gamma_mf : public estimator_base {
public:
	const int model_id = 10002;

	bool initialized;

	record_array * ptr_test_data;
	record_array * ptr_qual_data;

	mat U0;
	mat U1;
	rowvec U1_function_table;

	mat V;
	mat Y;

	// (F_i, n_user)
	mat A;
	// (F_j, n_movie)
	mat B;
	
	vec A_lambda;	
	vec B_lambda;

	// (F_i, t + MaxDate)
	mat A_function_table;

	// (F_j, t + MaxDate)
	mat B_function_table;

	mat A_timebin;
	mat B_timebin;

	uvec date_origin_user;
	uvec date_origin_movie;


	struct {
		vec U_col;
		vec A_function_val;
		vec B_function_val;
		vec Yi;
	} update_temp_var_thread[N_THREADS];

	vector<vector<unsigned int>> vote_list;

	double mu;
	double scale;

	unsigned int K;
	unsigned int F_i;
	unsigned int F_j;
	unsigned int D_u;
	unsigned int D_i;

	double lambda_factor;

	double U0_lambda;
	double U1_lambda;
	double V_lambda;
	double Y_lambda;

	double lambda;

	double learning_rate;
	double learning_rate_per_record;

	double learning_rate_mul;
	double learning_rate_min;


	gamma_mf() {
		K = 20;
		D_u = 20;
		D_i = 20;

		initialized = false;
		ptr_test_data = NULL;

		lambda_factor = 1;
		U0_lambda = 0.0001;
		U1_lambda = 0.1;
		V_lambda = 0.001;
		Y_lambda = 0.0001;

		lambda = 0.0001;

		// learning_rate = 0.002;
		learning_rate = 0.0015;

		learning_rate_mul = 0.9;
		learning_rate_min = 0;
	}

	virtual bool save(const char * file_name) {
		ofstream output_file(file_name, ios::binary | ios::out);
		output_file.write((const char *)&model_id, sizeof(int));
		output_file.write((const char *)&K, sizeof(int));
		U0.save(output_file);
		V.save(output_file);
		A.save(output_file);
		B.save(output_file);
		output_file << mu;
		return output_file.good();
	}

	virtual bool load(const char * file_name) {
		ifstream input_file(file_name, ios::binary | ios::in);
		int id;
		input_file.read((char *)&id, sizeof(int));
		input_file.read((char *)&K, sizeof(int));
		if (id != model_id) {
			cout << "FATAL: Loading Error" << endl;
			cout << model_id << ":" << id << endl;
			return false;
		}
		U0.load(input_file);
		V.load(input_file);
		A.load(input_file);
		B.load(input_file);
		input_file >> mu;
		initialized = true;
		return input_file.good();
	}

	void get_Y(vec& rtn, unsigned int i) {
		rtn.resize(K);
		rtn.fill(fill::zeros);
		vector<unsigned int>& user_vote_list = vote_list[i];
		for (unsigned int j : user_vote_list) {
			rtn += Y.unsafe_col(j);
		}
		if (user_vote_list.size() > 0) {
			rtn /= user_vote_list.size();
		}
	}

	void update_Y(unsigned int i, double r_pFpX, double *Vj, unsigned int K) {
		vector<unsigned int>& user_vote_list = vote_list[i];
		r_pFpX /= user_vote_list.size();
		for (unsigned int j : user_vote_list) {
			fang_add_mul(Y.colptr(j), Vj, r_pFpX, K);
		}
	}

	virtual float predict(const record & rcd) {
		unsigned int i = rcd.user - 1, j = rcd.movie - 1, d = rcd.date;
		unsigned int d_i = get_timebin(rcd.date, D_u), d_j = get_timebin(rcd.date, D_i);
		vec A_func_val = eval_functions(A_function_table, d - date_origin_user[i]);
		vec B_func_val = eval_functions(B_function_table, d - date_origin_movie[j]);
		vec U_col(K);
	    fang_add_mul_rtn(U_col, U0.colptr(i), U1.colptr(i), eval_function(U1_function_table, d - date_origin_user[i]), U0.n_rows);

		double result = mu + dot(U_col, V.unsafe_col(j));

		result += dot(A_func_val, A.col(i));
		result += dot(B_func_val, B.col(j));

		result += A_timebin(d_i, i);
		result += B_timebin(d_j, j);

#if _USE_Y
		vec Y_i;
		get_Y(Y_i, i);
		result += dot(V.unsafe_col(j), Y_i);
#endif


		if (result > 5) {
			result = 5;
		}
		if (result < 1) {
			result = 1;
		}
#ifdef _TEST_NAN
		if (isnan(result)) {
			cout << endl;
			cout << d << endl;
			cout << date_origin_user[i] << ' ' << date_origin_movie[j] << endl;
			cout << "A_func_val" << endl;
			cout << A_func_val << A.col(i);
			cout << "B_func_val" << endl;
			cout << B_func_val << B.col(j);
			cout << mu << ' ' << as_scalar(U0.col(i).t() * V.col(j));
		}
#endif
		return result;
	}

	void reshuffle(unsigned int *shuffle_idx, unsigned int n) {
		for (int i = n - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			swap(shuffle_idx[i], shuffle_idx[j]);
		}
	}


	void update(const record & rcd, unsigned int tid) {
		unsigned int i = rcd.user - 1, j = rcd.movie - 1, d = rcd.date;
		unsigned int d_i = get_timebin(rcd.date, D_u), d_j = get_timebin(rcd.date, D_i);

		double r_pFpX;

		double *U0i = U0.colptr(i);
		double *U1i = U1.colptr(i);
		double u = eval_function(U1_function_table, d - date_origin_user[i]);

		vec &U_col = update_temp_var_thread[tid].U_col;
		vec &A_func_val = update_temp_var_thread[tid].A_function_val;
		vec &B_func_val = update_temp_var_thread[tid].B_function_val;
		vec &Yi = update_temp_var_thread[tid].Yi;

		fang_add_mul_rtn(U_col, U0i, U1i, u, U0.n_rows);

		double *Vj = V.colptr(j);
		double UiVj = dot(U_col, V.unsafe_col(j));

		double data_mul = 1; // 0.2 * (2243 - rcd.date) / 2243 + 0.8;

		A_func_val = eval_functions(A_function_table, d - date_origin_user[i]);
		B_func_val = eval_functions(B_function_table, d - date_origin_movie[j]);

		double result = mu + UiVj;
		result += dot(A_func_val, A.unsafe_col(i));
		result += dot(B_func_val, B.unsafe_col(j));
		result += A_timebin(d_i, i);
		result += B_timebin(d_j, j);

#if _USE_Y
		get_Y(Yi, i);
		result += dot(V.unsafe_col(j), Yi);
#endif

#ifdef _TEST_NAN
		if (isnan(result)) {
			cout << result << endl;
			cout << mu << ' ' << UiVj << endl;
			cout << "A" << endl;
			cout << A_func_val;
			cout << A.col(i);
			cout << "B" << endl;
			cout << B_func_val;
			cout << B.col(j);
			cout << endl;
			cout << "Y" << endl;
			cout << Yi;
			cout << endl;
		}
#endif

		//learning rate * pFpX
		r_pFpX = data_mul * learning_rate_per_record * 2.0 * (rcd.score - result);

		// U(:,i) = U(:,i) - rate * gUi; gUi = - pFpX * V(:,j);
		fang_add_mul(U0i, Vj, r_pFpX, K);
		fang_add_mul(U1i, Vj, r_pFpX * u, K);

		// V(:,j) = V(:,j) - rate * gVj; gVj = - pFpX * U(:,i);
		fang_add_mul(Vj, U_col.memptr(), r_pFpX, K);

		// A(:,i) = A(:,i) - rate * gAi; gAi = - pFpX;
		fang_add_mul(A.colptr(i), A_func_val.memptr(), r_pFpX, A_func_val.n_rows);

		// B(:,j) = B(:,j) - rate * gBj; gBj = - pFpX;
		fang_add_mul(B.colptr(j), B_func_val.memptr(), r_pFpX, B_func_val.n_rows);

		A_timebin(d_i, i) += r_pFpX;
		B_timebin(d_j, j) += r_pFpX;

#if _USE_Y
		update_Y(i, r_pFpX, Vj, K);
#endif

	}

	void init(const record_array & train_data, unsigned int n_user, unsigned int n_movie) {
		// Set scale
		scale = 1;

		// Calculate mu
		unsigned int cnt[6];
		long long s;

		for (int i = 0; i < 6; i++) {
			cnt[i] = 0;
		}

		for (int i = 0; i < train_data.size; i++) {
			cnt[int(train_data[i].score)]++;
		}

		s = 0;
		for (int i = 0; i < 6; i++) {
			s += cnt[i] * i;
		}

		mu = 1.0 * s / train_data.size;

		// Calculate t_origin
		date_origin_user = uvec(n_user, fill::zeros);
		date_origin_movie = uvec(n_movie, fill::zeros);

		uvec user_count(n_user, fill::zeros);
		uvec movie_count(n_movie, fill::zeros);
		for (unsigned int index = 0; index < train_data.size; index++) {
			unsigned int i = train_data[index].user - 1;
			unsigned int j = train_data[index].movie - 1;
			unsigned int d = train_data[index].date;
			user_count[i]++;
			movie_count[j]++;
			date_origin_user[i] += d;
			date_origin_movie[j] += d;
		}

		// Handle _count == 0
		for (unsigned int i = 0; i < user_count.n_elem; i++)
		{
			if (user_count[i] == 0) {
				date_origin_user[i] = 2243 / 2;
			} else {
				date_origin_user[i] /= user_count[i];
			}

		}
		for (unsigned int i = 0; i < movie_count.n_elem; i++)
		{
			if (movie_count[i] == 0) {
				date_origin_movie[i] = 2243 / 2;
			} else {
				date_origin_movie[i] /= movie_count[i];
			}
		}



		// Reshape the matrix based on n_user and n_movie
		U0.set_size(K, n_user);
		U1.set_size(K, n_user);
		V.set_size(K, n_movie);
		Y.set_size(K, n_movie);

		A_timebin.set_size(D_u, n_user);
		B_timebin.set_size(D_i, n_movie);


		function_table_generator ftg;
		vector<double> A_lambda_raw;
		vector<double> B_lambda_raw;

		// U table
		U1_function_table = ftg.abspwr_table(0.4);

		// A table

		A_function_table.insert_rows(A_function_table.n_rows, ftg.const_table());
		A_lambda_raw.push_back(lambda);

		A_function_table.insert_rows(A_function_table.n_rows, ftg.abspwr_table(0.4));
		A_lambda_raw.push_back(lambda);

		A_function_table.insert_rows(A_function_table.n_rows, ftg.abspwr_table(1.2));
		A_lambda_raw.push_back(lambda);


		// B table
		
		B_function_table.insert_rows(B_function_table.n_rows, ftg.const_table());
		B_lambda_raw.push_back(lambda);

		B_function_table.insert_rows(B_function_table.n_rows, ftg.abspwr_table(0.4));
		B_lambda_raw.push_back(lambda);

		B_function_table.insert_rows(B_function_table.n_rows, ftg.abspwr_table(1.2));
		B_lambda_raw.push_back(lambda);

		vector<double> w_list = { 2.0 * MAX_DATE / 28, 2.0 * MAX_DATE / 7, 2.0 * MAX_DATE / 90, 0.25, 1, 4};
		for (int i = 0; i < w_list.size(); i++) {
			double w = w_list[i];
			A_function_table.insert_rows(A_function_table.n_rows, ftg.sinw_table(i));
			A_lambda_raw.push_back(lambda);

			A_function_table.insert_rows(A_function_table.n_rows, ftg.cosw_table(i));
			A_lambda_raw.push_back(lambda);

			B_function_table.insert_rows(B_function_table.n_rows, ftg.sinw_table(i));
			B_lambda_raw.push_back(lambda);

			B_function_table.insert_rows(B_function_table.n_rows, ftg.cosw_table(i));
			B_lambda_raw.push_back(lambda);
		}



		A.resize(A_function_table.n_rows, n_user);
		B.resize(B_function_table.n_rows, n_movie);

		A_lambda = vec(A_lambda_raw);
		B_lambda = vec(B_lambda_raw);
		
		U0.fill(fill::randu);
		U1.fill(fill::zeros);
		V.fill(fill::randu);
		Y.fill(fill::randu);

		A.fill(fill::randu);
		B.fill(fill::randu);
		A_timebin.fill(fill::zeros);
		B_timebin.fill(fill::zeros);

	}

	virtual void fit(const record_array & train_data, unsigned int n_iter = 1, bool continue_fit = false) {
		try {
			unsigned int batch_size = 1000;
			unsigned int block_size = train_data.size / batch_size / 16;
			unsigned int n_user = 0, n_movie = 0;
			unsigned int *shuffle_idx;
			unsigned int *shuffle_idx_batch[N_THREADS];
			timer tmr;


			tmr.display_mode = 1;
			learning_rate_per_record = learning_rate;

			// Calculate n_user and n_movies
			for (int i = 0; i < train_data.size; i++) {
				if (train_data[i].user > n_user) {
					n_user = train_data[i].user;
				}
				if (train_data[i].movie > n_movie) {
					n_movie = train_data[i].movie;
				}
			}

			// Generate shuffle_idx
			cout << train_data.size << endl;

			shuffle_idx = new unsigned int[train_data.size / batch_size];
			for (int i = 0; i < train_data.size / batch_size; i++) {
				shuffle_idx[i] = i;
			}

			for (int i = 0; i < N_THREADS; i++) {

				shuffle_idx_batch[i] = new unsigned int[batch_size];
				for (int j = 0; j < batch_size; j++) {
					shuffle_idx_batch[i][j] = j;
				}
			}

			if (!continue_fit) {
				init(train_data, n_user, n_movie);
			}


			// Regenerate U_col
			for (unsigned int i = 0; i < N_THREADS; i++) {
				update_temp_var_thread[i].U_col.resize(K);
			}

			tmr.tic();
			// Regenerate the list of movies voted by one user
			vote_list.resize(n_user);
			for (unsigned int i = 0; i < n_user; i++) {
				vote_list[i] = vector<unsigned int>();
			}
			for (unsigned int i = 0; i < train_data.size; i++) {
				vote_list[train_data[i].user - 1].push_back(train_data[i].movie - 1);
			}
			tmr.toc();

			for (int i_iter = 0; i_iter < n_iter; i_iter++) {

				tmr.tic();
				cout << "Iter\t" << i_iter << '\t';

				// Reshuffle first
				reshuffle(shuffle_idx, train_data.size / batch_size);

#pragma omp parallel for num_threads(N_THREADS)
				for (int i = 0; i < train_data.size / batch_size; i++) {
					unsigned int index_base = shuffle_idx[i] * batch_size;

					reshuffle(shuffle_idx_batch[omp_get_thread_num()], batch_size);

					for (int j = 0; j < batch_size; j++) {
						unsigned int index = index_base + shuffle_idx_batch[omp_get_thread_num()][j];

						// shuffle_idx_batch[j] do harm to the result
						if (index < train_data.size) {
							const record& rcd = train_data[index];
							update(rcd, omp_get_thread_num());
						}
					}

					if (i % block_size == 0) {
						cout << '.';
					}
				}
				if (ptr_test_data != NULL) {
					vector<float> result = this->predict_list(*ptr_test_data);
					cout << fixed;
					cout << setprecision(5);
					cout << '\t' << RMSE(*ptr_test_data, result);
					cout << '\t' << scale;
				}


				cout << '\t';
				tmr.toc();

				cout << "\t\t";
				cout << max(max(abs(U0))) << ' '
					<< max(max(abs(U1))) << ' '
					<< max(max(abs(V))) << ' '
					<< max(max(abs(Y))) << ' '
					<< max(max(abs(A))) << ' '
					<< max(max(abs(B))) << endl;

				if (ptr_qual_data != NULL) {
					auto result = this->predict_list(*ptr_qual_data);

					cout << "Writting output file" << endl;

					char buf[256];
					sprintf(buf, "output\\output_iter_%d.txt", i_iter);

					ofstream output_file(buf);

					if (!output_file.is_open()) {
						cerr << "Fail to open output file" << endl;
						system("pause");
						exit(-1);
					}

					for (int i = 0; i < result.size(); i++) {
						output_file << result[i] << endl;
					}
				}

				if (i_iter != n_iter - 1) {
					vec A_shrink(A.n_rows);
					vec B_shrink(B.n_rows);

					double regu_pwr = lambda_factor;
					// Recalculate all the shrinks
					for (unsigned int i = 0; i < A.n_rows; i++) {
						A_shrink[i] = pow(1 - A_lambda[i] * learning_rate_per_record, regu_pwr);
					}

					for (unsigned int i = 0; i < B.n_rows; i++) {
						B_shrink[i] = pow(1 - B_lambda[i] * learning_rate_per_record, regu_pwr);
					}

					// Regularization
					
					U0 *= pow(1 - U0_lambda * learning_rate_per_record, regu_pwr);
					U1 *= pow(1 - U1_lambda * learning_rate_per_record, regu_pwr);
					V *= pow(1 - V_lambda * learning_rate_per_record, regu_pwr);
					Y *= pow(1 - Y_lambda * learning_rate_per_record, regu_pwr);
					A_timebin *= pow(1 - lambda * learning_rate_per_record, regu_pwr);
					B_timebin *= pow(1 - lambda * learning_rate_per_record, regu_pwr);

					for (unsigned int j = 0; j < A.n_cols; j++) {
						A.col(j) %= A_shrink; // Element wise multiplication
					}
					for (unsigned int j = 0; j < B.n_cols; j++) {
						B.col(j) %= B_shrink; // Element wise multiplication
					}
					
					scale = scale * learning_rate_mul;
					learning_rate_per_record = learning_rate * scale;
				}
			}
			delete[]shuffle_idx;
		}
		catch (std::bad_alloc & ba) {
			cout << "bad_alloc caught: " << ba.what() << endl;
			system("pause");
		}
	}

};

#endif


