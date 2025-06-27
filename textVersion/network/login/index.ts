import { createId, database } from '../common';
import express from 'express';

export const login = (req: express.Request, res: express.Response) => {
    const { users, name_to_id } = database;

    console.log('Login request' + JSON.stringify(req.body));
    const name = req.body.name;
    let id = name_to_id[name];
    if (!id) {
        const player_id = createId();
        name_to_id[name] = player_id;
        users[player_id] = {
            name: name,
            id: player_id
        }
        id = player_id;
    }
    res.end(JSON.stringify({
        name: name,
        player_id: id
    }));

};