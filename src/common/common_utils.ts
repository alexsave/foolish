export const emailToName = (email: string): string => {
  return email.split('@')[0];
}
