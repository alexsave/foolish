import { useRouter } from "next/navigation";
import supabase from "../../backend/Connector";
import { TexturedSurface } from "../TexturedSurface";
import { Text } from "../Text";

export const CreateGameButton: React.FC = () => {
    const router = useRouter();

    const handleCreate = () => {
        supabase.functions.invoke('create')
            .then(data => {
                router.push(`/${data.data.id}`);
            }).catch(error => {
                alert(error);
            });
    };

    return (
        <TexturedSurface
            as="button"
            seed={0.7}
            onClick={handleCreate}
            className="btn-wood btn-wood--sm btn-wood--full"
        >
            <span className="btn-wood-text">
                <Text id="create_new_game" />
            </span>
        </TexturedSurface>
    );
};
