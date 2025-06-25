
// obviously we will actually have security and whatever
// but for now you just enter a name and hit enter

import { useState } from 'react';
import { useAuth } from '../contexts/AuthContext';

export const Login = () => {
  const [name, setName] = useState('');
  const [password, setPassword] = useState('');

  const {login} = useAuth();
  return (
    <div>
      <input type="text" value={name} onChange={(e) => setName(e.target.value)} />  
      <input type="text" value={password} onChange={(e) => setPassword(e.target.value)} />

      <button onClick={() => {
        login(name);
      }}>
        Login
      </button>
    </div>
  );
};