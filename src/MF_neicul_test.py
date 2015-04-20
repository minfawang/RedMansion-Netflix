import os
import numpy as np
from models.MF_neicul import MF_neicul

def main():
    data_dir = 'data/'
    output_dir = 'output/'
    
    main_path = os.path.join(data_dir, 'main_q.npy')
    probe_path = os.path.join(data_dir, 'probe_q.npy')
    qual_path = os.path.join(data_dir, 'qual_q.npy')
    
    main_data = np.load(main_path)
    qual_data = np.load(qual_path)
    
    estimator = MF_neicul()
    estimator.train(main_data[:, 0:2], main_data[:, 3], 3)
    
    qual_y = estimator.predict_list(qual_data[:, 0:3])
    
    output_path = os.path.join(output_dir, 'output.dta')
    with open(output_path, 'w') as output_file:
        output_file.write('\n'.join([str(y) for y in qual_y]))
        
    print 'done...'
    

if __name__ == '__main__':
    main()